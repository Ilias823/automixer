#pragma once

#include "PluginProcessor.h"

#include <juce_audio_utils/juce_audio_utils.h>

// Shows every live member of this instance's group: name, input level,
// applied gain and a "talking" light. This instance is highlighted.
class GroupMeters : public juce::Component
{
public:
    explicit GroupMeters (AutomixerProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    int liveCount = 0;

    // The members to show: this one first, then every other source once
    // (twin instances on the same track collapse into one row, preferring the
    // one that has a real track name over a host default like "Audio 2").
    juce::Array<int> visibleSlots() const;

    // Peers more than ~1 s away on the timeline are not part of this mix.
    int sameTimelineHops() const { return int (juce::jmax (8000.0, proc.getSampleRate()) / am::kHop); }

private:
    AutomixerProcessor& proc;
};

class AutomixerEditor : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    explicit AutomixerEditor (AutomixerProcessor&);
    ~AutomixerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void refreshPresetList();
    void showSaveRow (bool show);
    void styleBar (juce::Slider&, const juce::String& paramId);

    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    AutomixerProcessor& proc;
    juce::LookAndFeel_V4 lnf;

    juce::ComboBox group;
    juce::Label groupLabel, nameLabel, status;
    juce::ToggleButton automix { "Automix" };
    juce::ToggleButton link { "Link" };
    juce::TextEditor name;
    GroupMeters meters;
    juce::String meterSignature;
    std::array<Knob, 7> knobs;

    // Amount macro: gentle <-> aggressive, drives the detail controls.
    Knob amount;
    juce::Label gentleLabel, aggressiveLabel, detailsLabel;

    // Presets and reset.
    juce::ComboBox presetBox;
    juce::TextButton saveButton { "Save..." }, deleteButton { "Delete" }, resetButton { "Reset" };
    juce::TextEditor presetName;
    juce::TextButton saveOk { "Save" }, saveCancel { "Cancel" };
    juce::Label presetLabel;
    juce::Array<AutomixerProcessor::PresetInfo> presets;
    bool saving = false;

    // Version, "Support on Ko-fi" and "Check for Updates".
    juce::Label versionLabel, updateStatus;
    juce::TextButton donateButton { "Support on Ko-fi" };
    juce::TextButton updateButton { "Check for Updates" };
    juce::String downloadUrl;
    void checkForUpdates();

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> groupAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> automixAttachment, linkAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomixerEditor)
};
