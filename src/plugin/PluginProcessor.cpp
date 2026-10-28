#include "PluginProcessor.h"
#include "PluginEditor.h"

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
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "automix", 1 }, "Automix", true));
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
} // namespace

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

    channel.prepare (48000.0, 2048, 2);
    setLatencySamples (channel.latencySamples());
    syncMembership();
    startTimerHz (10);
}

AutomixerProcessor::~AutomixerProcessor()
{
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
    if (auto* ph = getPlayHead())
        if (auto info = ph->getPosition())
            if (auto t = info->getTimeInSamples())
            {
                pos = *t;
                fromHost = true;
            }
    hasHostPosition.store (fromHost);
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
            if (joinedSlot >= 0)
                channel.attach (b->layout(), joinedSlot);
        }
    }

    if (auto* bus = currentBus(); bus != nullptr && joinedSlot >= 0)
    {
        auto& s = bus->slots[joinedSlot];
        s.heartbeatNs.store (am::monotonicNs());
        s.flags.store (pAutomix->load() > 0.5f ? 1u : 0u);
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
}

juce::AudioProcessorEditor* AutomixerProcessor::createEditor()
{
    return new AutomixerEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AutomixerProcessor();
}
