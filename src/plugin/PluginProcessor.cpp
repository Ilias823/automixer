#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Macro.h"

#include <random>

namespace
{
constexpr int kStateVersion = 1;

juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;

    StringArray groups { "Off" };
    for (int g = 0; g < AutomixerProcessor::kNumGroups; ++g)
        groups.add (String::charToString (juce_wchar ('A' + g)));

    auto db = [] (float v, int) { return String (v, 1) + " dB"; };
    auto ms = [] (float v, int) { return String (roundToInt (v)) + " ms"; };

    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "group", 1 }, "Group", groups, 1));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "amount", 2 }, "Amount",
        NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " %"; })));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "automix", 1 }, "Automix", true));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "link", 3 }, "Link", true));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "weight", 1 }, "Weight",
        NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f, AudioParameterFloatAttributes().withStringFromValueFunction (db)));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "range", 1 }, "Range",
        NormalisableRange<float> (0.0f, 40.0f, 0.5f), 18.0f, AudioParameterFloatAttributes().withStringFromValueFunction (db)));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "sharpness", 1 }, "Focus",
        NormalisableRange<float> (1.0f, 4.0f, 0.1f), 3.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + "x"; })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "margin", 1 }, "Sensitivity",
        NormalisableRange<float> (0.0f, 20.0f, 0.5f), 6.0f, AudioParameterFloatAttributes().withStringFromValueFunction (db)));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "attack", 1 }, "Attack",
        NormalisableRange<float> (0.5f, 50.0f, 0.1f, 0.5f), 2.0f, AudioParameterFloatAttributes().withStringFromValueFunction (ms)));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "hold", 1 }, "Hold",
        NormalisableRange<float> (0.0f, 2000.0f, 1.0f, 0.5f), 0.0f, AudioParameterFloatAttributes().withStringFromValueFunction (ms)));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "release", 1 }, "Release",
        NormalisableRange<float> (10.0f, 2000.0f, 1.0f, 0.5f), 80.0f, AudioParameterFloatAttributes().withStringFromValueFunction (ms)));
    return l;
}

struct FactoryPreset
{
    const char* name;
    float amount;
    float marginDb;
};

// Factory presets are Amount settings (the detail controls follow the macro)
// plus a sensitivity for the room.
const FactoryPreset kFactoryPresets[] = {
    { "Default", 50.0f, 6.0f },
    { "Gentle (quiet room)", 20.0f, 6.0f },
    { "Aggressive (strong bleed)", 85.0f, 6.0f },
    { "Noisy room", 70.0f, 9.0f },
};

constexpr const char* kPresetExtension = ".amxpreset";
} // namespace

const juce::StringArray& AutomixerProcessor::linkedParamIds()
{
    static const juce::StringArray ids { "amount", "range", "sharpness", "margin", "attack", "hold", "release" };
    return ids;
}

const juce::StringArray& AutomixerProcessor::presetParamIds()
{
    static const juce::StringArray ids { "amount", "range", "sharpness", "margin", "weight", "attack", "hold", "release" };
    return ids;
}

AutomixerProcessor::AutomixerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::mono(), true)
                          .withOutput ("Output", juce::AudioChannelSet::mono(), true)),
      params (*this, nullptr, "AutomixerState", createLayout())
{
    std::random_device rd;
    while (instanceId == 0)
        instanceId = (uint64_t (rd()) << 32) ^ rd();

    pGroup = params.getRawParameterValue ("group");
    pAutomix = params.getRawParameterValue ("automix");
    pWeight = params.getRawParameterValue ("weight");
    pRange = params.getRawParameterValue ("range");
    pSharp = params.getRawParameterValue ("sharpness");
    pMargin = params.getRawParameterValue ("margin");
    pAttack = params.getRawParameterValue ("attack");
    pHold = params.getRawParameterValue ("hold");
    pRelease = params.getRawParameterValue ("release");
    pAmount = params.getRawParameterValue ("amount");
    pLink = params.getRawParameterValue ("link");
    params.addParameterListener ("amount", this);

    channel.prepare (48000.0, 2048, 2);
    setLatencySamples (channel.latencySamples());
    syncMembership();
    startTimerHz (10);
}

AutomixerProcessor::~AutomixerProcessor()
{
    params.removeParameterListener ("amount", this);
    cancelPendingUpdate();
    stopTimer();
    channel.detach();
    if (joinedGroup >= 0)
        buses[(size_t) joinedGroup]->releaseSlot (joinedSlot, instanceId);
}

bool AutomixerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    return in == out && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
}

void AutomixerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    channel.prepare (sampleRate, samplesPerBlock, getTotalNumInputChannels());
    channel.setParams (readParams());
    setLatencySamples (channel.latencySamples());
    fallbackPos = 0;
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        syncMembership();
}

am::ChannelParams AutomixerProcessor::readParams() const
{
    am::ChannelParams p;
    p.automix = pAutomix->load() > 0.5f;
    p.weightDb = pWeight->load();
    p.rangeDb = pRange->load();
    p.sharpness = pSharp->load();
    p.marginDb = pMargin->load();
    p.attackMs = pAttack->load();
    p.holdMs = pHold->load();
    p.releaseMs = pRelease->load();
    return p;
}

void AutomixerProcessor::runChannel (juce::AudioBuffer<float>& buffer, bool bypassed)
{
    juce::ScopedNoDenormals noDenormals;

    auto p = readParams();
    if (bypassed)
        p.automix = false;
    channel.setParams (p);

    int64_t pos = fallbackPos;
    bool fromHost = false;
    int playing = -1;
    if (auto* ph = getPlayHead())
        if (auto info = ph->getPosition())
        {
            playing = info->getIsPlaying() ? 1 : 0;
            if (auto t = info->getTimeInSamples())
            {
                pos = *t;
                fromHost = true;
            }
        }
    if (posLogOn.load (std::memory_order_relaxed))
    {
        const uint32_t w = posWrite.load (std::memory_order_relaxed);
        posLog[w % posLog.size()] = { pos, buffer.getNumSamples(), playing };
        posWrite.store (w + 1, std::memory_order_release);
    }
    hasHostPosition.store (fromHost);
    channel.setHostPlaying (playing != 0); // unknown (-1) counts as playing
    fallbackPos = pos + buffer.getNumSamples();

    channel.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples(), pos);
}

void AutomixerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    runChannel (buffer, false);
}

void AutomixerProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // Keep the latency when bypassed so the track stays in sync, and drop out
    // of the competition so the other mics behave as if this one is absent.
    runChannel (buffer, true);
}

void AutomixerProcessor::timerCallback()
{
    syncMembership();

    static const juce::File flag ("/tmp/amx_poslog");
    posLogOn.store (flag.exists());
    const uint32_t w = posWrite.load (std::memory_order_acquire);
    if (w != posRead)
    {
        juce::File out ("/tmp/amx_pos_" + juce::File::createLegalFileName (displayName()) + ".txt");
        juce::String text;
        for (; posRead != w; ++posRead)
        {
            const auto& e = posLog[posRead % posLog.size()];
            text << juce::String (e.pos) << " " << e.n << " " << e.playing << "\n";
        }
        out.appendText (text);
    }
}

void AutomixerProcessor::syncMembership()
{
    const int wanted = juce::roundToInt (pGroup->load()) - 1; // -1 = Off

    // Slot lost (taken over after a long stall)? Rejoin.
    if (joinedGroup >= 0 && joinedSlot >= 0
        && buses[(size_t) joinedGroup]->layout()->slots[joinedSlot].owner.load() != instanceId)
        joinedSlot = -1;

    if (wanted != joinedGroup || (wanted >= 0 && joinedSlot < 0))
    {
        channel.detach();
        if (joinedGroup >= 0 && joinedSlot >= 0)
            buses[(size_t) joinedGroup]->releaseSlot (joinedSlot, instanceId);
        joinedGroup = -1;
        joinedSlot = -1;

        if (wanted >= 0 && wanted < kNumGroups)
        {
            auto& b = buses[(size_t) wanted];
            if (b == nullptr)
                b = am::GroupBus::open (wanted);
            joinedGroup = wanted;
            joinedSlot = b->claimSlot (instanceId);
            linkPrimed = false; // take over (or seed) the new group's linked settings
            if (joinedSlot >= 0)
                channel.attach (b->layout(), joinedSlot);
        }
    }

    if (auto* bus = currentBus(); bus != nullptr && joinedSlot >= 0)
    {
        auto& s = bus->slots[joinedSlot];
        s.heartbeatNs.store (am::monotonicNs());
        am::GroupBus::setFlags (s, am::kFlagAutomix, pAutomix->load() > 0.5f ? am::kFlagAutomix : 0u);
        syncLinkedSettings (*bus, joinedSlot);

        // While we play, a live peer that is neither playing nor at our
        // timeline position is a deleted instance the host keeps for undo:
        // mark it so it stays hidden after the transport stops.
        if ((s.flags.load() & am::kFlagPlaying) != 0)
        {
            const uint64_t now = am::monotonicNs();
            const int hops = int (juce::jmax (8000.0, getSampleRate()) / am::kHop);
            for (int i = 0; i < am::kMaxSlots; ++i)
            {
                auto& peer = bus->slots[i];
                if (i == joinedSlot || ! am::GroupBus::isActiveMember (peer, now))
                    continue;
                if ((peer.flags.load() & am::kFlagPlaying) == 0 && ! am::GroupBus::isOnSameTimeline (peer, s, now, hops))
                    am::GroupBus::setFlags (peer, am::kFlagLeftBehind, am::kFlagLeftBehind);
            }
        }
        am::GroupBus::setName (s, displayName().toStdString());
    }
}

juce::String AutomixerProcessor::getUserName() const
{
    return params.state.getProperty ("userName").toString();
}

void AutomixerProcessor::setUserName (const juce::String& n)
{
    params.state.setProperty ("userName", n.trim(), nullptr);
}

juce::String AutomixerProcessor::displayName() const
{
    auto n = getUserName();
    if (n.isEmpty())
        n = hostTrackName;
    if (n.isEmpty())
        n = "Mic " + juce::String (joinedSlot + 1);
    return n;
}

void AutomixerProcessor::updateTrackProperties (const TrackProperties& p)
{
    auto name = p.name.value_or (juce::String());
    juce::MessageManager::callAsync ([safe = juce::WeakReference<AutomixerProcessor> (this), name]
    {
        if (auto* self = safe.get())
            self->hostTrackName = name;
    });
}

void AutomixerProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = params.copyState();
    state.setProperty ("stateVersion", kStateVersion, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void AutomixerProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName (params.state.getType()))
        return;
    params.replaceState (juce::ValueTree::fromXml (*xml));
    // The loaded detail values win over the macro; older projects without an
    // Amount keep their values too (Amount stays at its default).
    lastAppliedAmount.store (pAmount->load());
}

void AutomixerProcessor::parameterChanged (const juce::String&, float)
{
    // Any thread (also host automation): apply on the message thread.
    triggerAsyncUpdate();
}

void AutomixerProcessor::handleAsyncUpdate()
{
    const float amount = pAmount->load();
    if (std::abs (amount - lastAppliedAmount.load()) < 0.01f)
        return;
    lastAppliedAmount.store (amount);

    const auto v = am::macroValues (amount);
    setParamValue ("range", v.rangeDb);
    setParamValue ("sharpness", v.sharpness);
    setParamValue ("hold", v.holdMs);
    setParamValue ("release", v.releaseMs);
}

void AutomixerProcessor::setParamValue (const juce::String& id, float value)
{
    if (auto* p = params.getParameter (id))
    {
        const float norm = p->convertTo0to1 (value);
        if (std::abs (p->getValue() - norm) < 1e-6f)
            return;
        p->beginChangeGesture();
        p->setValueNotifyingHost (norm);
        p->endChangeGesture();
    }
}

std::vector<float> AutomixerProcessor::linkedSnapshot() const
{
    std::vector<float> v;
    for (const auto& id : linkedParamIds())
        if (auto* p = params.getParameter (id))
            v.push_back (p->convertFrom0to1 (p->getValue()));
    return v;
}

void AutomixerProcessor::syncLinkedSettings (am::BusLayout& bus, int slot)
{
    const auto cur = linkedSnapshot();
    const auto preset = getCurrentPresetName();
    const int n = (int) cur.size();

    if (pLink->load() < 0.5f)
    {
        linkPrimed = false; // switching Link on again takes over the group's settings
        lastLinked = cur;
        lastLinkedPreset = preset;
        return;
    }

    auto apply = [&] (const float* vals, const std::string& name)
    {
        std::map<juce::String, float> m;
        for (int i = 0; i < n; ++i)
            m[linkedParamIds()[i]] = vals[i];
        applyValues (m);
        params.state.setProperty ("presetName", juce::String (name), nullptr);
        lastLinked = linkedSnapshot();
        lastLinkedPreset = getCurrentPresetName();
    };

    auto publish = [&]
    {
        am::GroupBus::writeSettings (bus, instanceId, slot, cur.data(), n, preset.toStdString());
        lastSeenSettings = bus.settings.version.load();
        lastLinked = cur;
        lastLinkedPreset = preset;
    };

    float vals[am::kSharedValues] = {};
    uint64_t ver = 0, writer = 0;
    int writerSlot = -1;
    std::string name;
    const bool have = am::GroupBus::readSettings (bus, ver, writer, writerSlot, vals, n, name);
    const bool writerLive = have && writerSlot >= 0 && writerSlot < am::kMaxSlots
                            && bus.slots[writerSlot].owner.load() == writer
                            && am::GroupBus::isAlive (bus.slots[writerSlot], am::monotonicNs());

    if (! linkPrimed)
    {
        // Just joined, or Link was switched on: follow the group if a live
        // member set it, otherwise our values become the group's.
        linkPrimed = true;
        if (writerLive && writer != instanceId)
        {
            lastSeenSettings = ver;
            apply (vals, name);
        }
        else
        {
            publish();
        }
        return;
    }

    if (have && ver != lastSeenSettings && writer != instanceId)
    {
        lastSeenSettings = ver;
        apply (vals, name);
        return;
    }

    bool changed = preset != lastLinkedPreset || cur.size() != lastLinked.size();
    for (size_t i = 0; ! changed && i < cur.size(); ++i)
        changed = std::abs (cur[i] - lastLinked[i]) > 1e-4f * std::max (1.0f, std::abs (cur[i]));
    if (changed)
        publish();
}

void AutomixerProcessor::applyValues (const std::map<juce::String, float>& values)
{
    if (auto it = values.find ("amount"); it != values.end())
        lastAppliedAmount.store (it->second); // set first: the macro must not override these
    for (const auto& [id, value] : values)
        setParamValue (id, value);
}

void AutomixerProcessor::resetToDefaults()
{
    std::map<juce::String, float> values;
    for (const auto& id : presetParamIds())
        if (auto* p = params.getParameter (id))
            values[id] = p->convertFrom0to1 (p->getDefaultValue());
    applyValues (values);
    params.state.setProperty ("presetName", "Default", nullptr);
}

juce::File AutomixerProcessor::userPresetFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Application Support/Automixer/Presets");
}

juce::Array<AutomixerProcessor::PresetInfo> AutomixerProcessor::listPresets() const
{
    juce::Array<PresetInfo> out;
    for (const auto& f : kFactoryPresets)
        out.add ({ f.name, true });

    auto files = userPresetFolder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + kPresetExtension);
    files.sort();
    for (const auto& f : files)
        out.add ({ f.getFileNameWithoutExtension(), false });
    return out;
}

bool AutomixerProcessor::loadPreset (const juce::String& name)
{
    std::map<juce::String, float> values;

    for (const auto& f : kFactoryPresets)
    {
        if (name != f.name)
            continue;
        // Start from the defaults so a factory preset is always the same sound.
        for (const auto& id : presetParamIds())
            if (auto* p = params.getParameter (id))
                values[id] = p->convertFrom0to1 (p->getDefaultValue());
        const auto m = am::macroValues (f.amount);
        values["amount"] = f.amount;
        values["range"] = m.rangeDb;
        values["sharpness"] = m.sharpness;
        values["hold"] = m.holdMs;
        values["release"] = m.releaseMs;
        values["margin"] = f.marginDb;
        break;
    }

    if (values.empty())
    {
        auto xml = juce::XmlDocument::parse (userPresetFolder().getChildFile (name + kPresetExtension));
        if (xml == nullptr || ! xml->hasTagName ("AutomixerPreset"))
            return false;
        for (auto* e : xml->getChildWithTagNameIterator ("Param"))
        {
            const auto id = e->getStringAttribute ("id");
            if (presetParamIds().contains (id))
                values[id] = (float) e->getDoubleAttribute ("value");
        }
        if (values.empty())
            return false;
    }

    applyValues (values);
    params.state.setProperty ("presetName", name, nullptr);
    return true;
}

bool AutomixerProcessor::saveUserPreset (const juce::String& rawName)
{
    const auto name = juce::File::createLegalFileName (rawName.trim());
    if (name.isEmpty())
        return false;
    for (const auto& f : kFactoryPresets)
        if (name == f.name)
            return false; // factory names are reserved

    juce::XmlElement xml ("AutomixerPreset");
    xml.setAttribute ("version", 1);
    for (const auto& id : presetParamIds())
        if (auto* p = params.getParameter (id))
        {
            auto* e = xml.createNewChildElement ("Param");
            e->setAttribute ("id", id);
            e->setAttribute ("value", p->convertFrom0to1 (p->getValue()));
        }

    auto folder = userPresetFolder();
    if (! folder.createDirectory())
        return false;
    if (! xml.writeTo (folder.getChildFile (name + kPresetExtension)))
        return false;
    params.state.setProperty ("presetName", name, nullptr);
    return true;
}

bool AutomixerProcessor::deleteUserPreset (const juce::String& name)
{
    auto f = userPresetFolder().getChildFile (juce::File::createLegalFileName (name) + kPresetExtension);
    if (! f.existsAsFile() || ! f.deleteFile())
        return false;
    if (getCurrentPresetName() == name)
        params.state.setProperty ("presetName", "", nullptr);
    return true;
}

juce::AudioProcessorEditor* AutomixerProcessor::createEditor()
{
    return new AutomixerEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AutomixerProcessor();
}
