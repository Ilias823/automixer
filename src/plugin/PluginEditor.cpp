#include "PluginEditor.h"

namespace
{
const juce::Colour kBg { 0xff1c1f24 };
const juce::Colour kPanel { 0xff262a31 };
const juce::Colour kText { 0xffe6e8eb };
const juce::Colour kDim { 0xff8b929c };
const juce::Colour kAccent { 0xff4fb3a9 };
const juce::Colour kGain { 0xffe0a84f };

float dbToX (float db, float lo, float hi, juce::Rectangle<float> r)
{
    return r.getX() + r.getWidth() * juce::jlimit (0.0f, 1.0f, (db - lo) / (hi - lo));
}
} // namespace

void GroupMeters::paint (juce::Graphics& g)
{
    g.setColour (kPanel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);

    auto* bus = proc.currentBus();
    const int self = proc.currentSlot();
    auto area = getLocalBounds().reduced (10, 8);

    liveCount = 0;
    if (bus == nullptr)
    {
        g.setColour (kDim);
        g.setFont (14.0f);
        g.drawText ("Group is Off: this mic is not automixed.", area, juce::Justification::centred);
        return;
    }

    const uint64_t now = am::monotonicNs();
    const int rowH = 26;
    g.setFont (13.0f);

    for (int i = 0; i < am::kMaxSlots && area.getHeight() >= rowH; ++i)
    {
        const auto& s = bus->slots[i];
        if (! am::GroupBus::isActiveMember (s, now) && i != self)
            continue;
        ++liveCount;

        auto row = area.removeFromTop (rowH).toFloat();
        const bool isSelf = i == self;
        const bool enabled = (s.flags.load() & 1) != 0;
        const float level = s.meterLevelDb.load();
        const float gain = s.meterGainDb.load();
        const bool talking = am::entryDrive (s.latest.load()) > 0.0f && enabled;

        if (isSelf)
        {
            g.setColour (kAccent.withAlpha (0.12f));
            g.fillRoundedRectangle (row, 4.0f);
        }

        auto light = row.removeFromLeft (20).withSizeKeepingCentre (9, 9);
        g.setColour (talking ? kAccent : kDim.withAlpha (0.35f));
        g.fillEllipse (light);

        auto nameArea = row.removeFromLeft (120);
        g.setColour (enabled ? kText : kDim);
        juce::String n (am::GroupBus::getName (s));
        if (! enabled)
            n << " (off)";
        g.drawFittedText (n, nameArea.toNearestInt(), juce::Justification::centredLeft, 1);

        auto gainText = row.removeFromRight (64);
        g.setColour (kGain);
        g.drawText (juce::String (gain, 1) + " dB", gainText.toNearestInt(), juce::Justification::centredRight);

        // two thin bars: input level (-80..0 dBFS) and gain (-40..0 dB)
        row.removeFromRight (8);
        auto bars = row.reduced (0, 5);
        auto levelBar = bars.removeFromTop (bars.getHeight() / 2 - 1);
        bars.removeFromTop (2);
        auto gainBar = bars;

        g.setColour (kBg);
        g.fillRect (levelBar);
        g.fillRect (gainBar);
        g.setColour (kAccent.withAlpha (0.8f));
        g.fillRect (levelBar.withRight (dbToX (level, -80.0f, 0.0f, levelBar)));
        g.setColour (kGain);
        g.fillRect (gainBar.withRight (dbToX (gain, -40.0f, 0.0f, gainBar)));
    }
}

AutomixerEditor::AutomixerEditor (AutomixerProcessor& p)
    : AudioProcessorEditor (p), proc (p), meters (p)
{
    lnf.setColourScheme (juce::LookAndFeel_V4::getDarkColourScheme());
    lnf.setColour (juce::Slider::rotarySliderFillColourId, kAccent);
    lnf.setColour (juce::Slider::thumbColourId, kText);
    setLookAndFeel (&lnf);

    groupLabel.setText ("Group", juce::dontSendNotification);
    nameLabel.setText ("Name", juce::dontSendNotification);
    for (auto* l : { &groupLabel, &nameLabel })
    {
        l->setColour (juce::Label::textColourId, kDim);
        addAndMakeVisible (*l);
    }

    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (proc.params.getParameter ("group")))
        group.addItemList (choice->choices, 1);
    addAndMakeVisible (group);
    groupAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.params, "group", group);

    addAndMakeVisible (automix);
    automixAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.params, "automix", automix);

    name.setTextToShowWhenEmpty ("track name", kDim);
    name.setText (proc.getUserName(), false);
    name.onTextChange = [this] { proc.setUserName (name.getText()); };
    addAndMakeVisible (name);

    status.setColour (juce::Label::textColourId, kDim);
    status.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (status);

    addAndMakeVisible (meters);

    const std::array<std::pair<const char*, const char*>, 7> ids { {
        { "range", "Range" }, { "sharpness", "Focus" }, { "margin", "Sensitivity" }, { "weight", "Weight" },
        { "attack", "Attack" }, { "hold", "Hold" }, { "release", "Release" } } };

    for (size_t i = 0; i < knobs.size(); ++i)
    {
        auto& k = knobs[i];
        // Horizontal bars: drag left/right anywhere on the bar (the value moves
        // by the distance dragged, it never jumps to the click), double-click
        // to reset. Easier in a host window than knobs.
        k.slider.setSliderStyle (juce::Slider::LinearBar);
        k.slider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 0, 0);
        k.slider.setColour (juce::Slider::trackColourId, kAccent.withAlpha (0.55f));
        k.slider.setColour (juce::Slider::backgroundColourId, kPanel);
        k.slider.setColour (juce::Slider::textBoxTextColourId, kText);
        k.slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        k.slider.setSliderSnapsToMousePosition (false); // relative drag, no jump on click
        k.slider.setVelocityBasedMode (false);
        if (auto* prm = dynamic_cast<juce::RangedAudioParameter*> (proc.params.getParameter (ids[i].first)))
            k.slider.setDoubleClickReturnValue (true, prm->convertFrom0to1 (prm->getDefaultValue()));
        k.label.setText (ids[i].second, juce::dontSendNotification);
        k.label.setJustificationType (juce::Justification::centredLeft);
        k.label.setColour (juce::Label::textColourId, kDim);
        addAndMakeVisible (k.slider);
        addAndMakeVisible (k.label);
        k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.params, ids[i].first, k.slider);
    }

    setSize (600, 470);
    startTimerHz (15);
}

AutomixerEditor::~AutomixerEditor()
{
    setLookAndFeel (nullptr);
}

void AutomixerEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
    g.setColour (kText);
    g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    g.drawText ("AUTOMIXER", 16, 12, 160, 26, juce::Justification::centredLeft);
}

void AutomixerEditor::resized()
{
    auto r = getLocalBounds().reduced (16, 12);

    auto top = r.removeFromTop (26);
    top.removeFromLeft (140);
    groupLabel.setBounds (top.removeFromLeft (44));
    group.setBounds (top.removeFromLeft (64));
    top.removeFromLeft (12);
    automix.setBounds (top.removeFromLeft (90));
    top.removeFromLeft (8);
    nameLabel.setBounds (top.removeFromLeft (44));
    name.setBounds (top);

    r.removeFromTop (6);
    status.setBounds (r.removeFromTop (18));
    r.removeFromTop (4);

    // Controls: two columns of labelled bars under the meters.
    const int rowH = 30, rows = ((int) knobs.size() + 1) / 2;
    auto controls = r.removeFromBottom (rows * rowH);
    r.removeFromBottom (8);
    meters.setBounds (r.reduced (0, 4));

    const int colW = (controls.getWidth() - 16) / 2;
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        const int col = int (i) / rows, row = int (i) % rows;
        juce::Rectangle<int> cell (controls.getX() + col * (colW + 16), controls.getY() + row * rowH, colW, rowH);
        cell = cell.reduced (0, 3);
        knobs[i].label.setBounds (cell.removeFromLeft (86));
        knobs[i].slider.setBounds (cell);
    }
}

void AutomixerEditor::timerCallback()
{
    // Repaint the meters only when something visible changed (cheap check),
    // so the window stays responsive inside the host.
    juce::String sig;
    if (auto* bus = proc.currentBus())
    {
        const uint64_t now = am::monotonicNs();
        for (int i = 0; i < am::kMaxSlots; ++i)
        {
            const auto& sl = bus->slots[i];
            if (! am::GroupBus::isActiveMember (sl, now) && i != proc.currentSlot())
                continue;
            sig << i << ':' << juce::roundToInt (sl.meterLevelDb.load()) << ':'
                << juce::roundToInt (sl.meterGainDb.load() * 2.0f) << ':'
                << (am::entryDrive (sl.latest.load()) > 0.0f ? 1 : 0) << ':' << (int) sl.flags.load() << ';';
        }
    }
    if (sig != meterSignature)
    {
        meterSignature = sig;
        meters.repaint();
    }

    juce::String s;
    if (proc.currentBus() == nullptr)
        s = "Not linked";
    else if (proc.currentSlot() < 0)
        s = "Group full (16 mics max)";
    else
        s << meters.liveCount << (meters.liveCount == 1 ? " mic" : " mics") << " linked in group "
          << juce::String::charToString (juce::juce_wchar ('A' + juce::roundToInt (proc.params.getRawParameterValue ("group")->load()) - 1))
          << (proc.currentBusIsShared() ? "" : "  (process-local link)");
    if (! proc.hostGivesPosition())
        s << "  |  host gives no timeline position";
    s << "  |  latency " << juce::String (1000.0 * proc.getLatencySamples() / juce::jmax (1.0, proc.getSampleRate()), 1) << " ms";
    if (s != status.getText())
        status.setText (s, juce::dontSendNotification);

    if (! name.hasKeyboardFocus (true) && name.getText() != proc.getUserName())
        name.setText (proc.getUserName(), false);
}
