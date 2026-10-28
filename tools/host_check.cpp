// Loads the built Automixer.vst3 twice like a host would and checks that
//  1. the two instances link into one group,
//  2. gain-sharing works through the real plugin binary (talker open, other mic down),
//  3. saved state round-trips into a fresh instance.
//
//   host_check path/to/Automixer.vst3

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <cstdio>
#include <random>

namespace
{
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (! ok)
        ++failures;
}

struct TimelinePlayHead : juce::AudioPlayHead
{
    int64_t pos = 0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setTimeInSamples (pos);
        info.setIsPlaying (true);
        return info;
    }
};

juce::AudioProcessorParameter* findParam (juce::AudioPluginInstance& p, const juce::String& name)
{
    for (auto* prm : p.getParameters())
        if (prm->getName (64) == name)
            return prm;
    return nullptr;
}

double rmsDb (const juce::AudioBuffer<float>& b, int from, int to)
{
    double s = 0;
    for (int i = from; i < to; ++i)
        s += double (b.getSample (0, i)) * b.getSample (0, i);
    return 10.0 * std::log10 (s / std::max (1, to - from) + 1e-20);
}
} // namespace

int main (int argc, char** argv)
{
    // Own shared-memory buses: never link into a host session that is running.
    setenv ("AMX_BUS_NAMESPACE", "test", 1);
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 2)
    {
        std::fprintf (stderr, "usage: host_check Automixer.vst3\n");
        return 2;
    }

    juce::AudioPluginFormatManager fm;
    fm.addFormat (new juce::VST3PluginFormat());

    juce::OwnedArray<juce::PluginDescription> descs;
    juce::VST3PluginFormat vst3;
    vst3.findAllTypesForFile (descs, juce::String (argv[1]));
    if (descs.isEmpty())
    {
        std::fprintf (stderr, "no plugin found in %s\n", argv[1]);
        return 1;
    }
    std::printf ("loaded %s by %s, version %s\n", descs[0]->name.toRawUTF8(),
                 descs[0]->manufacturerName.toRawUTF8(), descs[0]->version.toRawUTF8());

    constexpr double sr = 48000.0;
    constexpr int block = 512;
    juce::String err;
    auto a = fm.createPluginInstance (*descs[0], sr, block, err);
    auto b = fm.createPluginInstance (*descs[0], sr, block, err);
    check (a != nullptr && b != nullptr, "two instances created " + err);
    if (a == nullptr || b == nullptr)
        return 1;

    TimelinePlayHead ph;
    for (auto* p : { a.get(), b.get() })
    {
        p->setPlayHead (&ph);
        p->setPlayConfigDetails (1, 1, sr, block);
        p->prepareToPlay (sr, block);
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300); // let instances join their group

    const int lat = a->getLatencySamples();
    check (lat > 0 && lat == b->getLatencySamples(), "latency reported: " + juce::String (lat) + " samples");

    // A talks for 3 s; B only hears A as -15 dB bleed. Both have room noise.
    const int len = int (sr * 3);
    juce::AudioBuffer<float> ia (1, len), ib (1, len);
    std::mt19937 rng (7);
    std::normal_distribution<float> nd (0.0f, 1.0f);
    for (int i = 0; i < len; ++i)
    {
        const float speech = (i > sr * 0.5) ? 0.1f * nd (rng) : 0.0f;
        ia.setSample (0, i, speech + 0.0005f * nd (rng));
        ib.setSample (0, i, 0.18f * speech + 0.0005f * nd (rng));
    }
    auto oa = ia, ob = ib;

    juce::MidiBuffer midi;
    for (int pos = 0; pos + block <= len; pos += block)
    {
        ph.pos = pos;
        juce::AudioBuffer<float> ba (oa.getArrayOfWritePointers(), 1, pos, block);
        juce::AudioBuffer<float> bb (ob.getArrayOfWritePointers(), 1, pos, block);
        a->processBlock (ba, midi);
        b->processBlock (bb, midi);
    }

    const int from = int (sr * 1.5), to = int (sr * 2.9);
    const double gA = rmsDb (oa, from + lat, to) - rmsDb (ia, from, to - lat);
    const double gB = rmsDb (ob, from + lat, to) - rmsDb (ib, from, to - lat);
    check (gA > -1.0, "talker mic stays open: " + juce::String (gA, 2) + " dB");
    check (gB < -15.0, "bleed mic pulled down: " + juce::String (gB, 2) + " dB");

    // State round trip: change parameters on A, save, load into a fresh instance.
    auto* range = findParam (*a, "Range");
    auto* group = findParam (*a, "Group");
    check (range != nullptr && group != nullptr, "parameters exposed to host");
    range->setValueNotifyingHost (range->getValueForText ("24.0"));
    group->setValueNotifyingHost (group->getValueForText ("C"));
    {
        // VST3 hosts deliver parameter changes with the next process call.
        juce::AudioBuffer<float> tmp (1, block);
        tmp.clear();
        a->processBlock (tmp, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    }
    std::printf ("INFO  set on A: Range %s, Group %s\n", range->getCurrentValueAsText().toRawUTF8(), group->getCurrentValueAsText().toRawUTF8());
    juce::MemoryBlock state;
    a->getStateInformation (state);

    auto c = fm.createPluginInstance (*descs[0], sr, block, err);
    c->setStateInformation (state.getData(), (int) state.getSize());
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    auto* cr = findParam (*c, "Range");
    auto* cg = findParam (*c, "Group");
    check (cr->getCurrentValueAsText() == range->getCurrentValueAsText(),
           "Range restored: " + cr->getCurrentValueAsText());
    check (cg->getCurrentValueAsText() == "C", "Group restored: " + cg->getCurrentValueAsText());

    // Amount macro: moving it sets the detail controls (applied on the message
    // thread), but loading saved state must keep the saved detail values.
    {
        auto* amount = findParam (*a, "Amount");
        check (amount != nullptr, "Amount parameter exposed to host");
        if (amount != nullptr)
        {
            amount->setValueNotifyingHost (amount->getValueForText ("100"));
            for (int k = 0; k < 3; ++k)
            {
                juce::AudioBuffer<float> tmp (1, block);
                tmp.clear();
                a->processBlock (tmp, midi);
                juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
            }
            check (range->getCurrentValueAsText() == "30.0 dB", "Amount 100 % sets Range: " + range->getCurrentValueAsText());
            auto* hold = findParam (*a, "Hold");
            check (hold->getCurrentValueAsText() == "0 ms", "Amount 100 % sets Hold: " + hold->getCurrentValueAsText());

            // Saved state with a hand-tuned Range must come back as saved.
            range->setValueNotifyingHost (range->getValueForText ("12.0"));
            juce::AudioBuffer<float> tmp (1, block);
            tmp.clear();
            a->processBlock (tmp, midi);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
            juce::MemoryBlock st;
            a->getStateInformation (st);
            auto d = fm.createPluginInstance (*descs[0], sr, block, err);
            d->setStateInformation (st.getData(), (int) st.getSize());
            juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
            check (findParam (*d, "Range")->getCurrentValueAsText() == "12.0 dB",
                   "hand-tuned Range survives reload with Amount 100 %: " + findParam (*d, "Range")->getCurrentValueAsText());
            d->releaseResources();
        }
    }

    // Link: two fresh instances in one group share the linked settings; Weight
    // stays per mic; with Link off a mic keeps its own values.
    {
        auto x = fm.createPluginInstance (*descs[0], sr, block, err);
        auto y = fm.createPluginInstance (*descs[0], sr, block, err);
        auto pump = [&]
        {
            for (int k = 0; k < 4; ++k)
            {
                juce::AudioBuffer<float> tmp (1, block);
                tmp.clear();
                x->processBlock (tmp, midi);
                y->processBlock (tmp, midi);
                juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
            }
        };
        for (auto* p : { x.get(), y.get() })
        {
            p->setPlayHead (&ph);
            p->setPlayConfigDetails (1, 1, sr, block);
            p->prepareToPlay (sr, block);
            findParam (*p, "Group")->setValueNotifyingHost (findParam (*p, "Group")->getValueForText ("H"));
        }
        pump();

        findParam (*x, "Range")->setValueNotifyingHost (findParam (*x, "Range")->getValueForText ("27.0"));
        findParam (*x, "Weight")->setValueNotifyingHost (findParam (*x, "Weight")->getValueForText ("4.0"));
        pump();
        check (findParam (*y, "Range")->getCurrentValueAsText() == "27.0 dB",
               "Link on: Range follows the other mic: " + findParam (*y, "Range")->getCurrentValueAsText());
        check (findParam (*y, "Weight")->getCurrentValueAsText() == "0.0 dB",
               "Link on: Weight stays per mic: " + findParam (*y, "Weight")->getCurrentValueAsText());

        findParam (*y, "Link")->setValueNotifyingHost (0.0f);
        pump();
        findParam (*x, "Range")->setValueNotifyingHost (findParam (*x, "Range")->getValueForText ("10.0"));
        pump();
        check (findParam (*y, "Range")->getCurrentValueAsText() == "27.0 dB",
               "Link off: mic keeps its own Range: " + findParam (*y, "Range")->getCurrentValueAsText());

        findParam (*y, "Link")->setValueNotifyingHost (1.0f);
        pump();
        check (findParam (*y, "Range")->getCurrentValueAsText() == "10.0 dB",
               "Link on again: mic takes the group's Range: " + findParam (*y, "Range")->getCurrentValueAsText());
        x->releaseResources();
        y->releaseResources();
    }

    for (auto* p : { a.get(), b.get(), c.get() })
        p->releaseResources();

    std::printf ("\n%s (%d failed)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
