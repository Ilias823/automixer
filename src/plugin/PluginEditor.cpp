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

juce::Array<int> GroupMeters::visibleSlots() const
{
    juce::Array<int> out;
    auto* bus = proc.currentBus();
    const int self = proc.currentSlot();
    if (bus == nullptr || self < 0)
        return out;

    const uint64_t now = am::monotonicNs();
    const auto& me = bus->slots[self];
    out.add (self);

    auto isDefaultName = [] (const std::string& n)
    {
        return juce::String (n).matchesWildcard ("Audio *", true) && juce::String (n).fromFirstOccurrenceOf (" ", false, false).containsOnly ("0123456789");
    };

    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < am::kMaxSlots; ++i)
        {
            if (out.contains (i))
                continue;
            const auto& s = bus->slots[i];
            if (! am::GroupBus::isOnSameTimeline (s, me, now, sameTimelineHops()))
                continue;
            if ((pass == 0) == isDefaultName (am::GroupBus::getName (s)))
                continue; // real names first, host defaults second
            bool twin = false;
            for (int j : out)
                twin = twin || am::GroupBus::isTwin (bus->slots[j], s) || am::GroupBus::isTwin (s, bus->slots[j]);
            if (! twin)
                out.add (i);
        }
    return out;
}

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

    const int rowH = 26;
    g.setFont (13.0f);

    for (int i : visibleSlots())
    {
        if (area.getHeight() < rowH)
            break;
        const auto& s = bus->slots[i];
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
        styleBar (k.slider, ids[i].first);
        k.label.setText (ids[i].second, juce::dontSendNotification);
        k.label.setJustificationType (juce::Justification::centredLeft);
        k.label.setColour (juce::Label::textColourId, kDim);
        addAndMakeVisible (k.slider);
        addAndMakeVisible (k.label);
        k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.params, ids[i].first, k.slider);
    }

    // Amount: one bar that sets Range, Focus, Hold and Release together.
    styleBar (amount.slider, "amount");
    amount.slider.setColour (juce::Slider::trackColourId, kGain.withAlpha (0.65f));
    amount.label.setText ("Amount", juce::dontSendNotification);
    amount.label.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    amount.label.setColour (juce::Label::textColourId, kText);
    addAndMakeVisible (amount.slider);
    addAndMakeVisible (amount.label);
    amount.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.params, "amount", amount.slider);

    gentleLabel.setText ("gentle", juce::dontSendNotification);
    aggressiveLabel.setText ("aggressive", juce::dontSendNotification);
    aggressiveLabel.setJustificationType (juce::Justification::centredRight);
    detailsLabel.setText ("Details (Amount sets Range, Focus, Hold and Release; fine-tune here)", juce::dontSendNotification);
    for (auto* l : { &gentleLabel, &aggressiveLabel, &detailsLabel })
    {
        l->setFont (juce::FontOptions (11.5f));
        l->setColour (juce::Label::textColourId, kDim);
        addAndMakeVisible (*l);
    }

    // Presets.
    presetLabel.setText ("Preset", juce::dontSendNotification);
    presetLabel.setColour (juce::Label::textColourId, kDim);
    addAndMakeVisible (presetLabel);
    presetBox.setTextWhenNothingSelected ("(no preset)");
    presetBox.onChange = [this]
    {
        const int idx = presetBox.getSelectedId() - 1;
        if (idx >= 0 && idx < presets.size() && presets[idx].name != proc.getCurrentPresetName())
            proc.loadPreset (presets[idx].name);
        deleteButton.setEnabled (idx >= 0 && idx < presets.size() && ! presets[idx].factory);
    };
    addAndMakeVisible (presetBox);

    saveButton.onClick = [this] { showSaveRow (true); };
    deleteButton.onClick = [this]
    {
        const int idx = presetBox.getSelectedId() - 1;
        if (idx >= 0 && idx < presets.size() && ! presets[idx].factory)
            proc.deleteUserPreset (presets[idx].name);
        refreshPresetList();
    };
    resetButton.setTooltip ("Set every value back to its default (Group, Automix and Name stay)");
    resetButton.onClick = [this]
    {
        proc.resetToDefaults();
        refreshPresetList();
    };
    for (auto* b : { &saveButton, &deleteButton, &resetButton })
        addAndMakeVisible (*b);

    presetName.setTextToShowWhenEmpty ("preset name", kDim);
    presetName.onReturnKey = [this] { saveOk.triggerClick(); };
    presetName.onEscapeKey = [this] { showSaveRow (false); };
    saveOk.onClick = [this]
    {
        if (proc.saveUserPreset (presetName.getText()))
        {
            showSaveRow (false);
            refreshPresetList();
        }
        else
        {
            presetName.setColour (juce::TextEditor::outlineColourId, juce::Colours::indianred);
            presetName.repaint();
        }
    };
    saveCancel.onClick = [this] { showSaveRow (false); };
    for (auto* c : std::initializer_list<juce::Component*> { &presetName, &saveOk, &saveCancel })
        addChildComponent (*c);

    refreshPresetList();

    setSize (600, 560);
    startTimerHz (15);
}

AutomixerEditor::~AutomixerEditor()
{
    setLookAndFeel (nullptr);
}

void AutomixerEditor::styleBar (juce::Slider& sl, const juce::String& paramId)
{
    // Horizontal bars: drag left/right anywhere on the bar (the value moves
    // by the distance dragged, it never jumps to the click), double-click
    // to reset. Easier in a host window than knobs.
    sl.setSliderStyle (juce::Slider::LinearBar);
    sl.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 0, 0);
    sl.setTextBoxIsEditable (false); // a click on the bar must drag, never open a number field
    sl.setColour (juce::Slider::trackColourId, kAccent.withAlpha (0.55f));
    sl.setColour (juce::Slider::backgroundColourId, kPanel);
    sl.setColour (juce::Slider::textBoxTextColourId, kText);
    sl.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    sl.setSliderSnapsToMousePosition (false);
    sl.setVelocityBasedMode (false);
    if (auto* prm = dynamic_cast<juce::RangedAudioParameter*> (proc.params.getParameter (paramId)))
        sl.setDoubleClickReturnValue (true, prm->convertFrom0to1 (prm->getDefaultValue()));
}

void AutomixerEditor::refreshPresetList()
{
    presets = proc.listPresets();
    presetBox.clear (juce::dontSendNotification);
    bool userHeading = false;
    presetBox.addSectionHeading ("Factory");
    for (int i = 0; i < presets.size(); ++i)
    {
        if (! presets[i].factory && ! userHeading)
        {
            presetBox.addSeparator();
            presetBox.addSectionHeading ("My presets");
            userHeading = true;
        }
        presetBox.addItem (presets[i].name, i + 1);
    }

    const auto current = proc.getCurrentPresetName();
    int sel = 0;
    for (int i = 0; i < presets.size(); ++i)
        if (presets[i].name == current)
            sel = i + 1;
    presetBox.setSelectedId (sel, juce::dontSendNotification);
    deleteButton.setEnabled (sel > 0 && ! presets[sel - 1].factory);
}

void AutomixerEditor::showSaveRow (bool show)
{
    saving = show;
    for (auto* c : std::initializer_list<juce::Component*> { &presetBox, &saveButton, &deleteButton, &resetButton })
        c->setVisible (! show);
    for (auto* c : std::initializer_list<juce::Component*> { &presetName, &saveOk, &saveCancel })
        c->setVisible (show);
    if (show)
    {
        presetName.setText (proc.getCurrentPresetName(), false);
        presetName.removeColour (juce::TextEditor::outlineColourId);
        presetName.grabKeyboardFocus();
        presetName.selectAll();
    }
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
    r.removeFromTop (6);

    // Preset row (or the inline save row in its place).
    auto presetRow = r.removeFromTop (26);
    presetLabel.setBounds (presetRow.removeFromLeft (54));
    auto right = presetRow.removeFromRight (230);
    resetButton.setBounds (right.removeFromRight (70));
    right.removeFromRight (6);
    deleteButton.setBounds (right.removeFromRight (70));
    right.removeFromRight (6);
    saveButton.setBounds (right.removeFromRight (72));
    presetBox.setBounds (presetRow.withTrimmedRight (6));
    auto saveRow = presetLabel.getBounds().getUnion (resetButton.getBounds()).withTrimmedLeft (54);
    saveCancel.setBounds (saveRow.removeFromRight (70));
    saveRow.removeFromRight (6);
    saveOk.setBounds (saveRow.removeFromRight (70));
    saveRow.removeFromRight (6);
    presetName.setBounds (saveRow);
    r.removeFromTop (10);

    // Amount macro.
    auto amountRow = r.removeFromTop (34);
    amount.label.setBounds (amountRow.removeFromLeft (86));
    amount.slider.setBounds (amountRow);
    auto captions = r.removeFromTop (16).withTrimmedLeft (86);
    gentleLabel.setBounds (captions.removeFromLeft (captions.getWidth() / 2));
    aggressiveLabel.setBounds (captions);
    r.removeFromTop (4);

    // Detail controls: two columns of labelled bars at the bottom.
    const int rowH = 30, rows = ((int) knobs.size() + 1) / 2;
    auto controls = r.removeFromBottom (rows * rowH);
    detailsLabel.setBounds (r.removeFromBottom (18));
    r.removeFromBottom (2);
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
        for (int i : meters.visibleSlots())
        {
            const auto& sl = bus->slots[i];
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

    // Keep the preset box in step with state loaded or changed elsewhere.
    if (! saving)
    {
        const int sel = presetBox.getSelectedId();
        const auto shown = sel > 0 && sel <= presets.size() ? presets[sel - 1].name : juce::String();
        if (shown != proc.getCurrentPresetName())
            refreshPresetList();
    }
}
