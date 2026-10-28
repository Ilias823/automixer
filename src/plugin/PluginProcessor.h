#pragma once

#include "AutomixChannel.h"
#include "GroupBus.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <memory>

class AutomixerProcessor : public juce::AudioProcessor,
                           private juce::Timer
{
public:
    static constexpr int kNumGroups = 8;

    AutomixerProcessor();
    ~AutomixerProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Automixer"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    void updateTrackProperties (const TrackProperties&) override;

    // For the editor (message thread).
    juce::AudioProcessorValueTreeState params;
    am::BusLayout* currentBus() const { return joinedGroup >= 0 ? buses[(size_t) joinedGroup]->layout() : nullptr; }
    int currentSlot() const { return joinedSlot; }
    bool currentBusIsShared() const { return joinedGroup >= 0 && buses[(size_t) joinedGroup]->isShared(); }
    bool hostGivesPosition() const { return hasHostPosition.load(); }
    juce::String displayName() const;
    void setUserName (const juce::String&);
    juce::String getUserName() const;

private:
    void timerCallback() override;
    void syncMembership();
    am::ChannelParams readParams() const;
    void runChannel (juce::AudioBuffer<float>&, bool bypassed);

    am::AutomixChannel channel;
    uint64_t instanceId = 0;

    std::array<std::unique_ptr<am::GroupBus>, kNumGroups> buses;
    int joinedGroup = -1; // message thread only
    int joinedSlot = -1;

    juce::String hostTrackName; // message thread only

    std::atomic<bool> hasHostPosition { true };

    // Debug: when /tmp/amx_poslog exists, block positions are logged and the
    // timer writes them to /tmp/amx_pos_<name>.txt.
    struct PosEntry { int64_t pos; int n; int playing; };
    std::array<PosEntry, 8192> posLog {};
    std::atomic<uint32_t> posWrite { 0 };
    uint32_t posRead = 0;
    std::atomic<bool> posLogOn { false };
    int64_t fallbackPos = 0;

    std::atomic<float>* pGroup = nullptr;
    std::atomic<float>* pAutomix = nullptr;
    std::atomic<float>* pWeight = nullptr;
    std::atomic<float>* pRange = nullptr;
    std::atomic<float>* pSharp = nullptr;
    std::atomic<float>* pMargin = nullptr;
    std::atomic<float>* pAttack = nullptr;
    std::atomic<float>* pHold = nullptr;
    std::atomic<float>* pRelease = nullptr;

    JUCE_DECLARE_WEAK_REFERENCEABLE (AutomixerProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomixerProcessor)
};
