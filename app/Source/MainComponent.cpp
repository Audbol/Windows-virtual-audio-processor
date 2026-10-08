#include "MainComponent.h"

namespace vb
{

namespace
{
    const juce::Colour kBackground  { 0xff12151a };
    const juce::Colour kPanel       { 0xff1c2129 };
    const juce::Colour kAccent      { 0xff4fa3ff };
    const juce::Colour kText        { 0xffe6e9ef };
    const juce::Colour kDimText     { 0xff8b94a3 };

    constexpr const char* kAutoTargetId = "auto";
    constexpr const char* kDriverTargetId = "driver";
    constexpr const char* kOffTargetId = "off";

    void styleHeader (juce::Label& l)
    {
        l.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        l.setColour (juce::Label::textColourId, kAccent);
    }

    void styleSlider (juce::Slider& s, double min, double max, double interval, const juce::String& suffix)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 72, 22);
        s.setRange (min, max, interval);
        s.setTextValueSuffix (suffix);
    }
}

//==============================================================================
class PluginWindow final : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioProcessor& p, std::function<void()> onCloseRequested)
        : juce::DocumentWindow (p.getName(), kBackground, juce::DocumentWindow::closeButton),
          onClose (std::move (onCloseRequested))
    {
        setUsingNativeTitleBar (true);

        juce::AudioProcessorEditor* editor = p.hasEditor() ? p.createEditorIfNeeded() : nullptr;
        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor (p);

        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        toFront (true);
    }

    ~PluginWindow() override
    {
        clearContentComponent();   // deletes the editor while the processor is still alive
    }

    void closeButtonPressed() override
    {
        if (onClose != nullptr)
            onClose();
    }

private:
    std::function<void()> onClose;
};

//==============================================================================
class MainComponent::ChainRow final : public juce::Component
{
public:
    explicit ChainRow (MainComponent& o) : owner (o)
    {
        for (auto* c : std::initializer_list<juce::Component*> { &power, &name, &latency, &edit, &up, &down, &remove })
            addAndMakeVisible (c);

        power.setTooltip ("Enable / bypass");
        name.setColour (juce::Label::textColourId, kText);
        name.setInterceptsMouseClicks (false, false);
        latency.setColour (juce::Label::textColourId, kDimText);
        latency.setJustificationType (juce::Justification::centredRight);
        latency.setInterceptsMouseClicks (false, false);
        up.setTooltip ("Move up");
        down.setTooltip ("Move down");
        remove.setTooltip ("Remove");

        power.onClick  = [this] { owner.toggleBypass (row); };
        edit.onClick   = [this] { owner.openEditor (row); };
        up.onClick     = [this] { owner.moveSlot (row, -1); };
        down.onClick   = [this] { owner.moveSlot (row, +1); };
        remove.onClick = [this] { owner.removeSlot (row); };
    }

    void update (int newRow, const PluginChain::Slot& slot, double sampleRate)
    {
        row = newRow;
        const bool active = ! slot.bypassed.load();
        power.setToggleState (active, juce::dontSendNotification);
        name.setText (juce::String (row + 1) + ".  " + slot.description.name
                        + (slot.description.manufacturerName.isNotEmpty() ? "   (" + slot.description.manufacturerName + ")" : juce::String()),
                      juce::dontSendNotification);
        name.setAlpha (active ? 1.0f : 0.45f);

        const int lat = slot.plugin->getLatencySamples();
        latency.setText (lat > 0 && sampleRate > 0 ? juce::String (lat * 1000.0 / sampleRate, 1) + " ms" : juce::String(),
                         juce::dontSendNotification);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (kPanel.brighter (0.06f));
        g.fillRoundedRectangle (getLocalBounds().reduced (2, 2).toFloat(), 4.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 4);
        power.setBounds (r.removeFromLeft (28));
        remove.setBounds (r.removeFromRight (30));
        r.removeFromRight (4);
        down.setBounds (r.removeFromRight (30));
        up.setBounds (r.removeFromRight (30));
        r.removeFromRight (4);
        edit.setBounds (r.removeFromRight (60));
        latency.setBounds (r.removeFromRight (70));
        name.setBounds (r);
    }

private:
    MainComponent& owner;
    int row = 0;
    juce::ToggleButton power;
    juce::Label name, latency;
    juce::TextButton edit { "Edit" }, up { juce::String::fromUTF8 ("\xe2\x96\xb2") },
                     down { juce::String::fromUTF8 ("\xe2\x96\xbc") }, remove { juce::String::fromUTF8 ("\xe2\x9c\x95") };
};

//==============================================================================
MainComponent::MainComponent (AudioEngine& e, juce::ApplicationProperties& p)
    : engine (e), props (p)
{
    title.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, kText);
    addAndMakeVisible (title);

    for (auto* b : { &audioSettingsButton, &savePresetButton, &loadPresetButton })
        addAndMakeVisible (b);
    audioSettingsButton.onClick = [this] { showAudioSettings(); };
    savePresetButton.onClick = [this] { savePreset(); };
    loadPresetButton.onClick = [this] { loadPreset(); };

    // ---- input ----
    for (auto* h : { &inputHeader, &chainHeader, &monitorHeader, &vmHeader })
    {
        styleHeader (*h);
        addAndMakeVisible (h);
    }

    deviceLabel.setColour (juce::Label::textColourId, kDimText);
    addAndMakeVisible (deviceLabel);

    inputModeBox.addItem ("Mono - input 1 (mic / guitar)", 1);
    inputModeBox.addItem ("Mono - input 2", 2);
    inputModeBox.addItem ("Mono - sum of inputs", 3);
    inputModeBox.addItem ("Stereo - inputs 1 + 2", 4);
    inputModeBox.onChange = [this] { engine.setInputMode ((AudioEngine::InputMode) (inputModeBox.getSelectedId() - 1)); };
    addAndMakeVisible (inputModeBox);

    styleSlider (inputGain, -24.0, 24.0, 0.1, " dB");
    inputGain.onValueChange = [this] { engine.setInputGainDb ((float) inputGain.getValue()); };
    inputGain.setDoubleClickReturnValue (true, 0.0);
    addAndMakeVisible (inputGain);
    addAndMakeVisible (inputMeter);

    // ---- chain ----
    chainList.setRowHeight (40);
    chainList.setColour (juce::ListBox::backgroundColourId, kPanel);
    chainList.setOutlineThickness (0);
    addAndMakeVisible (chainList);

    emptyChainHint.setText ("No plugins yet. Click \"+ Add Plugin\" (scan your VST3 folders first) - "
                            "e.g. a noise gate, EQ, compressor, de-esser and reverb.",
                            juce::dontSendNotification);
    emptyChainHint.setJustificationType (juce::Justification::centred);
    emptyChainHint.setColour (juce::Label::textColourId, kDimText);
    emptyChainHint.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (emptyChainHint);

    for (auto* b : { &addPluginButton, &scanButton, &addFileButton })
        addAndMakeVisible (b);
    addPluginButton.onClick = [this] { showAddPluginMenu(); };
    scanButton.onClick = [this] { showPluginScanner(); };
    addFileButton.onClick = [this] { addPluginFromFile(); };

    // ---- monitor ----
    monitorToggle.onClick = [this] { engine.setMonitorEnabled (monitorToggle.getToggleState()); };
    addAndMakeVisible (monitorToggle);
    styleSlider (monitorGain, -60.0, 6.0, 0.1, " dB");
    monitorGain.setDoubleClickReturnValue (true, 0.0);
    monitorGain.onValueChange = [this] { engine.setMonitorGainDb ((float) monitorGain.getValue()); };
    addAndMakeVisible (monitorGain);
    addAndMakeVisible (outputMeter);
    latencyLabel.setColour (juce::Label::textColourId, kDimText);
    addAndMakeVisible (latencyLabel);

    // ---- virtual mic ----
    vmTargetBox.onChange = [this]
    {
        const int index = vmTargetBox.getSelectedId() - 1;
        if (juce::isPositiveAndBelow (index, vmTargetIds.size()))
            applyVirtualMicTarget (vmTargetIds[index]);
    };
    addAndMakeVisible (vmTargetBox);

    vmMuteToggle.onClick = [this] { engine.getVirtualMic().setMuted (vmMuteToggle.getToggleState()); };
    addAndMakeVisible (vmMuteToggle);

    styleSlider (vmGain, -24.0, 12.0, 0.1, " dB");
    vmGain.setDoubleClickReturnValue (true, 0.0);
    vmGain.onValueChange = [this] { engine.getVirtualMic().setGain (juce::Decibels::decibelsToGain ((float) vmGain.getValue())); };
    addAndMakeVisible (vmGain);

    styleSlider (vmSafety, 0.5, 30.0, 0.5, " ms");
    vmSafety.setTooltip ("Extra buffering between this app and the virtual mic. Lower = less latency; "
                         "raise it if the underrun counter keeps climbing.");
    vmSafety.onValueChange = [this] { engine.getVirtualMic().setSafetyMs (vmSafety.getValue()); };
    addAndMakeVisible (vmSafety);
    vmSafetyLabel.setColour (juce::Label::textColourId, kDimText);
    addAndMakeVisible (vmSafetyLabel);

    vmStatusLabel.setColour (juce::Label::textColourId, kText);
    vmStatsLabel.setColour (juce::Label::textColourId, kDimText);
    addAndMakeVisible (vmStatusLabel);
    addAndMakeVisible (vmStatsLabel);

    engine.getChain().addChangeListener (this);
    engine.getKnownPlugins().addChangeListener (this);
    engine.getDeviceManager().addChangeListener (this);

    restoreState();

    setSize (940, 680);
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    engine.getChain().removeChangeListener (this);
    engine.getKnownPlugins().removeChangeListener (this);
    engine.getDeviceManager().removeChangeListener (this);
    closeAllEditors();
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);

    auto drawPanel = [&] (juce::Rectangle<int> r)
    {
        g.setColour (kPanel);
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
    };

    for (auto* header : { &inputHeader, &chainHeader, &monitorHeader, &vmHeader })
    {
        auto panel = header->getBounds();
        if (header == &inputHeader)   panel = panel.getUnion (inputMeter.getBounds()).getUnion (deviceLabel.getBounds());
        if (header == &chainHeader)   panel = panel.getUnion (addPluginButton.getBounds()).getUnion (chainList.getBounds());
        if (header == &monitorHeader) panel = panel.getUnion (latencyLabel.getBounds()).getUnion (outputMeter.getBounds());
        if (header == &vmHeader)      panel = panel.getUnion (vmStatsLabel.getBounds()).getUnion (vmSafety.getBounds());
        drawPanel (panel.expanded (8, 6));
    }

    // Virtual mic status dot
    auto dot = vmStatusLabel.getBounds().withWidth (10).withSizeKeepingCentre (10, 10).translated (-14, 0);
    g.setColour (vmStatusColour);
    g.fillEllipse (dot.toFloat());
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (16);

    auto header = area.removeFromTop (34);
    title.setBounds (header.removeFromLeft (200));
    loadPresetButton.setBounds (header.removeFromRight (120).reduced (2));
    savePresetButton.setBounds (header.removeFromRight (120).reduced (2));
    audioSettingsButton.setBounds (header.removeFromRight (140).reduced (2));
    area.removeFromTop (14);

    // Input
    {
        inputHeader.setBounds (area.removeFromTop (18));
        deviceLabel.setBounds (area.removeFromTop (20));
        auto row = area.removeFromTop (28);
        inputModeBox.setBounds (row.removeFromLeft (260).reduced (0, 2));
        row.removeFromLeft (12);
        inputGain.setBounds (row.removeFromLeft (300));
        row.removeFromLeft (12);
        inputMeter.setBounds (row.reduced (0, 8));
    }
    area.removeFromTop (22);

    // Virtual mic (bottom)
    auto vm = area.removeFromBottom (112);
    {
        vmHeader.setBounds (vm.removeFromTop (18));
        auto row = vm.removeFromTop (30);
        vmTargetBox.setBounds (row.removeFromLeft (360).reduced (0, 2));
        row.removeFromLeft (12);
        vmMuteToggle.setBounds (row.removeFromLeft (70));
        vmGain.setBounds (row.removeFromLeft (260));
        vm.removeFromTop (4);
        auto row2 = vm.removeFromTop (26);
        vmSafetyLabel.setBounds (row2.removeFromLeft (100));
        vmSafety.setBounds (row2.removeFromLeft (260));
        row2.removeFromLeft (30);
        vmStatusLabel.setBounds (row2);
        vmStatsLabel.setBounds (vm.removeFromTop (22));
    }
    area.removeFromBottom (22);

    // Monitor
    auto mon = area.removeFromBottom (76);
    {
        monitorHeader.setBounds (mon.removeFromTop (18));
        auto row = mon.removeFromTop (28);
        monitorToggle.setBounds (row.removeFromLeft (140));
        monitorGain.setBounds (row.removeFromLeft (300));
        row.removeFromLeft (12);
        outputMeter.setBounds (row.reduced (0, 8));
        latencyLabel.setBounds (mon.removeFromTop (22));
    }
    area.removeFromBottom (22);

    // Chain (fills the rest)
    chainHeader.setBounds (area.removeFromTop (18));
    auto buttons = area.removeFromBottom (30);
    addPluginButton.setBounds (buttons.removeFromLeft (140).reduced (0, 2));
    buttons.removeFromLeft (8);
    scanButton.setBounds (buttons.removeFromLeft (160).reduced (0, 2));
    buttons.removeFromLeft (8);
    addFileButton.setBounds (buttons.removeFromLeft (150).reduced (0, 2));
    area.removeFromBottom (6);
    chainList.setBounds (area);
    emptyChainHint.setBounds (area);
}

//==============================================================================
int MainComponent::getNumRows()
{
    return engine.getChain().size();
}

juce::Component* MainComponent::refreshComponentForRow (int row, bool, juce::Component* existing)
{
    auto* slot = engine.getChain().getSlot (row);
    if (slot == nullptr)
    {
        delete existing;
        return nullptr;
    }

    auto* rowComp = dynamic_cast<ChainRow*> (existing);
    if (rowComp == nullptr)
    {
        delete existing;
        rowComp = new ChainRow (*this);
    }

    double rate = 0;
    if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
        rate = device->getCurrentSampleRate();

    rowComp->update (row, *slot, rate);
    return rowComp;
}

void MainComponent::toggleBypass (int row)
{
    if (auto* slot = engine.getChain().getSlot (row))
        engine.getChain().setBypassed (row, ! slot->bypassed.load());
}

void MainComponent::openEditor (int row)
{
    auto* slot = engine.getChain().getSlot (row);
    if (slot == nullptr)
        return;

    auto* processor = slot->plugin.get();

    if (auto it = editors.find (processor); it != editors.end())
    {
        it->second->toFront (true);
        return;
    }

    juce::Component::SafePointer<MainComponent> safeThis (this);
    editors[processor] = std::make_unique<PluginWindow> (*processor, [safeThis, processor]
    {
        // Defer: we're inside the window's own callback.
        juce::MessageManager::callAsync ([safeThis, processor]
        {
            if (safeThis != nullptr)
                safeThis->closeEditorFor (processor);
        });
    });
}

void MainComponent::removeSlot (int row)
{
    if (auto* slot = engine.getChain().getSlot (row))
    {
        closeEditorFor (slot->plugin.get());   // editor must go before its processor
        engine.getChain().removePlugin (row);
    }
}

void MainComponent::moveSlot (int row, int delta)
{
    engine.getChain().movePlugin (row, row + delta);
}

void MainComponent::closeEditorFor (juce::AudioProcessor* processor)
{
    editors.erase (processor);
}

void MainComponent::closeAllEditors()
{
    editors.clear();
}

//==============================================================================
void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &engine.getChain())
    {
        chainList.updateContent();
        chainList.repaint();
        emptyChainHint.setVisible (engine.getChain().size() == 0);
    }
    else if (source == &engine.getKnownPlugins())
    {
        if (auto xml = engine.getKnownPlugins().createXml())
            props.getUserSettings()->setValue ("knownPlugins", xml.get());
        props.saveIfNeeded();
    }
    else if (source == &engine.getDeviceManager())
    {
        updateStatus();
        chainList.updateContent();
    }
}

void MainComponent::timerCallback()
{
    inputMeter.pushPeak (engine.popInputPeak());
    outputMeter.pushPeak (engine.popOutputPeak());

    if (--statusCountdown <= 0)
    {
        statusCountdown = 10;
        updateStatus();
    }

    // Auto mode: if nothing usable was found (or the driver went away), look again
    // every few seconds so installing VB-CABLE / the driver is picked up live.
    if (currentVmTarget == kAutoTargetId && --autoRetryCountdown <= 0)
    {
        autoRetryCountdown = 30 * 5;
        auto& vm = engine.getVirtualMic();
        if (! vm.hasBackend() || vm.getStatus().state == VirtualMicBackend::State::unavailable)
            applyVirtualMicTarget (kAutoTargetId, true);
    }
}

void MainComponent::updateStatus()
{
    auto& dm = engine.getDeviceManager();
    if (dm.getCurrentAudioDevice() != nullptr)
    {
        const auto setup = dm.getAudioDeviceSetup();
        juce::String text = dm.getCurrentAudioDeviceType() + "  |  in: " + setup.inputDeviceName;
        if (setup.outputDeviceName.isNotEmpty())
            text << "  |  out: " << setup.outputDeviceName;
        deviceLabel.setText (text, juce::dontSendNotification);

        const auto lat = engine.getLatencyInfo();
        juce::String l;
        l << juce::String (lat.sampleRate / 1000.0, 1) << " kHz, " << lat.bufferSize << " samples ("
          << juce::String (lat.bufferSize * 1000.0 / juce::jmax (1.0, lat.sampleRate), 1) << " ms)"
          << "   |   monitor round-trip ~" << juce::String (lat.monitorRoundTripMs, 1) << " ms"
          << " (in " << juce::String (lat.inputMs, 1) << " + out " << juce::String (lat.outputMs, 1)
          << " + plugins " << juce::String (lat.pluginMs, 1) << ")";
        latencyLabel.setText (l, juce::dontSendNotification);
    }
    else
    {
        deviceLabel.setText ("No audio device open - click \"Audio Settings...\"", juce::dontSendNotification);
        latencyLabel.setText ({}, juce::dontSendNotification);
    }

    auto& vm = engine.getVirtualMic();
    const auto status = vm.getStatus();
    const bool autoFoundNothing = currentVmTarget == kAutoTargetId && ! vm.hasBackend();

    vmStatusLabel.setText (autoFoundNothing ? juce::String ("No virtual mic found - see below")
                                            : status.text,
                           juce::dontSendNotification);

    using State = VirtualMicBackend::State;
    vmStatusColour = autoFoundNothing                 ? juce::Colour (0xffe74c3c)
                   : ! vm.hasBackend()                ? juce::Colours::grey
                   : status.state == State::streaming ? juce::Colour (0xff2ecc71)
                   : status.state == State::idle      ? juce::Colour (0xfff1c40f)
                                                      : juce::Colour (0xffe74c3c);

    const juce::String pickHint = status.recordFrom.isNotEmpty()
                                    ? "In games / Discord / OBS select \"" + status.recordFrom + "\" as the microphone"
                                    : juce::String();

    if (autoFoundNothing)
    {
        vmStatsLabel.setText ("Install the VocalBridge driver, or a Microsoft-signed virtual cable such as VB-CABLE "
                              "(vb-audio.com/Cable - works with Secure Boot on). It will be picked up automatically.",
                              juce::dontSendNotification);
    }
    else if (vm.hasBackend() && status.state == State::streaming)
    {
        vmStatsLabel.setText ((pickHint.isNotEmpty() ? pickHint + "   |   " : juce::String())
                              + "added ~" + juce::String (vm.getEstimatedLatencyMs(), 1) + " ms"
                              + "   |   underruns: " + juce::String (status.underruns)
                              + "   |   resyncs: " + juce::String (status.resyncs),
                              juce::dontSendNotification);
    }
    else if (vm.hasBackend() && status.state == State::idle)
    {
        vmStatsLabel.setText ("Ready - " + (pickHint.isNotEmpty() ? pickHint : juce::String ("select the virtual mic in your game or app")) + ".",
                              juce::dontSendNotification);
    }
    else
    {
        vmStatsLabel.setText ({}, juce::dontSendNotification);
    }

    repaint (vmStatusLabel.getBounds().expanded (20, 0));
}

//==============================================================================
void MainComponent::showAudioSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent (engine.getDeviceManager(),
                                                             1, 16,     // inputs: pick your mic channel(s)
                                                             0, 16,     // outputs: headphones for monitoring
                                                             false, false,
                                                             false,     // show individual channels, not pairs
                                                             false);
    selector->setSize (560, 520);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (selector);
    o.dialogTitle = "Audio Settings";
    o.dialogBackgroundColour = kBackground;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void MainComponent::showPluginScanner()
{
    auto deadMansPedal = props.getUserSettings()->getFile().getSiblingFile ("PluginScanCrashList.txt");

    auto* list = new juce::PluginListComponent (engine.getFormats(), engine.getKnownPlugins(),
                                                deadMansPedal, props.getUserSettings(), false);
    list->setSize (760, 520);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (list);
    o.dialogTitle = "Plugins - use Options > \"Scan for new or updated VST3 plug-ins\"";
    o.dialogBackgroundColour = kBackground;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void MainComponent::showAddPluginMenu()
{
    auto types = engine.getKnownPlugins().getTypes();

    if (types.isEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "No plugins found",
                                                "Click \"Scan for Plugins...\" to scan your VST3 folders "
                                                "(usually C:\\Program Files\\Common Files\\VST3), or use \"Add VST3 File...\".");
        return;
    }

    juce::PopupMenu menu;
    juce::KnownPluginList::addToMenu (menu, types, juce::KnownPluginList::sortByManufacturer);

    juce::Component::SafePointer<MainComponent> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addPluginButton),
                        [safeThis, types] (int result)
                        {
                            if (safeThis == nullptr || result == 0)
                                return;

                            const int index = juce::KnownPluginList::getIndexChosenByMenu (types, result);
                            if (juce::isPositiveAndBelow (index, types.size()))
                                safeThis->addPlugin (types.getReference (index));
                        });
}

void MainComponent::addPlugin (const juce::PluginDescription& desc)
{
    double rate = 48000.0;
    int block = 512;
    if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
    {
        rate = device->getCurrentSampleRate();
        block = device->getCurrentBufferSizeSamples();
    }

    juce::String error;
    auto instance = engine.getFormats().createPluginInstance (desc, rate, block, error);

    if (instance == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Could not load plugin",
                                                desc.name + "\n\n" + error);
        return;
    }

    engine.getChain().addPlugin (std::move (instance), desc);
    openEditor (engine.getChain().size() - 1);
}

void MainComponent::addPluginFromFile()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Choose a VST3 plugin",
                                                       juce::File ("C:\\Program Files\\Common Files\\VST3"),
                                                       "*.vst3");

    juce::Component::SafePointer<MainComponent> safeThis (this);
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                | juce::FileBrowserComponent::canSelectDirectories,
                              [safeThis] (const juce::FileChooser& chooser)
    {
        if (safeThis == nullptr)
            return;

        const auto file = chooser.getResult();
        if (file == juce::File())
            return;

        juce::AudioPluginFormat* vst3 = nullptr;
        for (auto* format : safeThis->engine.getFormats().getFormats())
            if (format->getName() == "VST3")
                vst3 = format;

        if (vst3 == nullptr)
            return;

        juce::OwnedArray<juce::PluginDescription> found;
        safeThis->engine.getKnownPlugins().scanAndAddFile (file.getFullPathName(), false, found, *vst3);

        if (found.isEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Not a VST3 plugin",
                                                    file.getFullPathName());
            return;
        }

        safeThis->addPlugin (*found.getFirst());
    });
}

//==============================================================================
void MainComponent::savePreset()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Save chain preset",
                                                       juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                           .getChildFile ("VocalBridge Preset.vbpreset"),
                                                       "*.vbpreset");

    juce::Component::SafePointer<MainComponent> safeThis (this);
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safeThis] (const juce::FileChooser& chooser)
    {
        if (safeThis == nullptr || chooser.getResult() == juce::File())
            return;

        auto xml = safeThis->engine.getChain().createStateXml();
        xml->setTagName ("VOCALBRIDGE_PRESET");
        xml->writeTo (chooser.getResult().withFileExtension ("vbpreset"));
    });
}

void MainComponent::loadPreset()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Load chain preset",
                                                       juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                       "*.vbpreset");

    juce::Component::SafePointer<MainComponent> safeThis (this);
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safeThis] (const juce::FileChooser& chooser)
    {
        if (safeThis == nullptr || chooser.getResult() == juce::File())
            return;

        auto xml = juce::XmlDocument::parse (chooser.getResult());
        if (xml == nullptr)
            return;

        safeThis->closeAllEditors();
        juce::StringArray errors;
        safeThis->engine.getChain().restoreFromXml (*xml, safeThis->engine.getFormats(), errors);

        if (! errors.isEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Some plugins could not be loaded",
                                                    errors.joinIntoString ("\n"));
    });
}

//==============================================================================
void MainComponent::rebuildVirtualMicTargets()
{
    vmTargetBox.clear (juce::dontSendNotification);
    vmTargetIds.clear();

    auto add = [this] (const juce::String& id, const juce::String& label)
    {
        vmTargetIds.add (id);
        vmTargetBox.addItem (label, vmTargetIds.size());
    };

    add (kAutoTargetId, "Auto (recommended): VocalBridge driver, else a signed virtual cable");
    add (kOffTargetId, "Off");
#if JUCE_WINDOWS
    add (kDriverTargetId, "VocalBridge Virtual Mic (driver, lowest latency)");
#endif

    // Fallback targets: any output device (e.g. a signed virtual cable such as
    // VB-Audio "CABLE Input"). ASIO is skipped: one ASIO driver per process.
    vmTargetBox.addSeparator();
    for (auto* type : engine.getDeviceManager().getAvailableDeviceTypes())
    {
        if (type->getTypeName() == "ASIO")
            continue;

        type->scanForDevices();
        for (auto& name : type->getDeviceNames (false))
            add ("device|" + type->getTypeName() + "|" + name, "Output device: " + name + "  [" + type->getTypeName() + "]");
    }
}

std::unique_ptr<VirtualMicBackend> MainComponent::createAutoBackend()
{
#if JUCE_WINDOWS
    // 1. Our own driver: lowest latency (shared ring, no extra audio stream).
    if (DriverBackend::isDriverPresent())
        return std::make_unique<DriverBackend>();
#endif

    // 2. A Microsoft-signed third-party virtual cable: loads with Secure Boot on and
    //    no test mode, so it also works on anti-cheat-protected games.
    if (auto cable = DeviceBackend::openBestVirtualCable (engine.getDeviceManager()))
        return cable;

    return nullptr;
}

void MainComponent::applyVirtualMicTarget (const juce::String& id, bool force)
{
    if (! force && id == currentVmTarget && engine.getVirtualMic().hasBackend() == (id != kOffTargetId))
        return;

    currentVmTarget = id;
    std::unique_ptr<VirtualMicBackend> backend;

    if (id == kAutoTargetId)
    {
        // Release the current device first so the cable can be reopened in exclusive mode.
        engine.getVirtualMic().setBackend (nullptr);
        backend = createAutoBackend();
    }

#if JUCE_WINDOWS
    if (id == kDriverTargetId)
        backend = std::make_unique<DriverBackend>();
#endif

    if (id.startsWith ("device|"))
    {
        const auto parts = juce::StringArray::fromTokens (id.fromFirstOccurrenceOf ("device|", false, false), "|", {});
        if (parts.size() >= 2)
            backend = std::make_unique<DeviceBackend> (engine.getDeviceManager(), parts[0],
                                                       id.fromFirstOccurrenceOf ("device|" + parts[0] + "|", false, false));
    }

    engine.getVirtualMic().setBackend (std::move (backend));

    const int index = vmTargetIds.indexOf (id);
    vmTargetBox.setSelectedId (index >= 0 ? index + 1 : 1, juce::dontSendNotification);
    updateStatus();
}

//==============================================================================
void MainComponent::restoreState()
{
    auto* s = props.getUserSettings();

    if (auto xml = s->getXmlValue ("knownPlugins"))
        engine.getKnownPlugins().recreateFromXml (*xml);

    inputModeBox.setSelectedId (s->getIntValue ("inputMode", 0) + 1, juce::sendNotificationSync);
    inputGain.setValue (s->getDoubleValue ("inputGainDb", 0.0), juce::sendNotificationSync);
    monitorToggle.setToggleState (s->getBoolValue ("monitorOn", true), juce::sendNotificationSync);
    engine.setMonitorEnabled (monitorToggle.getToggleState());
    monitorGain.setValue (s->getDoubleValue ("monitorDb", 0.0), juce::sendNotificationSync);
    vmGain.setValue (s->getDoubleValue ("vmGainDb", 0.0), juce::sendNotificationSync);
    vmSafety.setValue (s->getDoubleValue ("vmSafetyMs", 3.0), juce::sendNotificationSync);
    vmMuteToggle.setToggleState (s->getBoolValue ("vmMuted", false), juce::sendNotificationSync);
    engine.getVirtualMic().setMuted (vmMuteToggle.getToggleState());

    if (auto xml = s->getXmlValue ("chain"))
    {
        juce::StringArray errors;
        engine.getChain().restoreFromXml (*xml, engine.getFormats(), errors);

        if (! errors.isEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Some plugins could not be loaded",
                                                    errors.joinIntoString ("\n"));
    }

    chainList.updateContent();
    emptyChainHint.setVisible (engine.getChain().size() == 0);

    rebuildVirtualMicTargets();
    const juce::String defaultTarget = kAutoTargetId;
    auto target = s->getValue ("vmTarget", defaultTarget);
    if (! vmTargetIds.contains (target))
        target = defaultTarget;
    applyVirtualMicTarget (target);
}

void MainComponent::saveState()
{
    auto* s = props.getUserSettings();

    if (auto xml = engine.getDeviceManager().createStateXml())
        s->setValue ("audioDevice", xml.get());
    if (auto xml = engine.getKnownPlugins().createXml())
        s->setValue ("knownPlugins", xml.get());
    if (auto xml = engine.getChain().createStateXml())
        s->setValue ("chain", xml.get());

    s->setValue ("inputMode", inputModeBox.getSelectedId() - 1);
    s->setValue ("inputGainDb", inputGain.getValue());
    s->setValue ("monitorOn", monitorToggle.getToggleState());
    s->setValue ("monitorDb", monitorGain.getValue());
    s->setValue ("vmTarget", currentVmTarget);
    s->setValue ("vmGainDb", vmGain.getValue());
    s->setValue ("vmSafetyMs", vmSafety.getValue());
    s->setValue ("vmMuted", vmMuteToggle.getToggleState());

    props.saveIfNeeded();
}

} // namespace vb
