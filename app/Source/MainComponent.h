#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "AudioEngine.h"

namespace vb
{

//==============================================================================
class LevelMeter final : public juce::Component
{
public:
    void pushPeak (float peak)
    {
        level = juce::jmax (peak, level * 0.82f);
        if (peak > hold) { hold = peak; holdFrames = 30; }
        else if (--holdFrames < 0) hold *= 0.9f;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff1b1f24));
        g.fillRoundedRectangle (r, 3.0f);

        auto toX = [&] (float lin)
        {
            const float db = juce::Decibels::gainToDecibels (lin, -60.0f);
            return juce::jmap (db, -60.0f, 0.0f, 0.0f, r.getWidth());
        };

        const float w = toX (level);
        juce::ColourGradient grad (juce::Colour (0xff2ecc71), r.getX(), 0, juce::Colour (0xffe74c3c), r.getRight(), 0, false);
        grad.addColour (0.75, juce::Colour (0xfff1c40f));
        g.setGradientFill (grad);
        g.fillRoundedRectangle (r.withWidth (w), 3.0f);

        g.setColour (hold >= 1.0f ? juce::Colours::red : juce::Colours::white.withAlpha (0.7f));
        g.fillRect (juce::Rectangle<float> (r.getX() + toX (hold) - 1.0f, r.getY(), 2.0f, r.getHeight()));
    }

private:
    float level = 0.0f, hold = 0.0f;
    int holdFrames = 0;
};

//==============================================================================
class PluginWindow;

class MainComponent final : public juce::Component,
                            private juce::Timer,
                            private juce::ChangeListener,
                            private juce::ListBoxModel
{
public:
    MainComponent (AudioEngine& engine, juce::ApplicationProperties& properties);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Persists device, plugin list, chain and UI settings. */
    void saveState();

    // Called by chain rows
    void toggleBypass (int row);
    void openEditor (int row);
    void removeSlot (int row);
    void moveSlot (int row, int delta);

private:
    class ChainRow;

    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem (int, juce::Graphics&, int, int, bool) override {}
    juce::Component* refreshComponentForRow (int row, bool selected, juce::Component* existing) override;

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void showAudioSettings();
    void showAddPluginMenu();
    void showPluginScanner();
    void addPluginFromFile();
    void addPlugin (const juce::PluginDescription&);
    void savePreset();
    void loadPreset();

    void rebuildVirtualMicTargets();
    void applyVirtualMicTarget (const juce::String& id, bool force = false);
    std::unique_ptr<VirtualMicBackend> createAutoBackend();

    void closeEditorFor (juce::AudioProcessor*);
    void closeAllEditors();

    void restoreState();
    void updateStatus();

    AudioEngine& engine;
    juce::ApplicationProperties& props;

    // Header
    juce::Label title { {}, "VocalBridge" };
    juce::TextButton audioSettingsButton { "Audio Settings..." }, savePresetButton { "Save Preset..." }, loadPresetButton { "Load Preset..." };

    // Input
    juce::Label inputHeader { {}, "INPUT" }, deviceLabel;
    juce::ComboBox inputModeBox;
    juce::Slider inputGain;
    LevelMeter inputMeter;

    // Chain
    juce::Label chainHeader { {}, "VST CHAIN" };
    juce::ListBox chainList { "chain", this };
    juce::TextButton addPluginButton { "+ Add Plugin" }, scanButton { "Scan for Plugins..." }, addFileButton { "Add VST3 File..." };
    juce::Label emptyChainHint;

    // Monitor
    juce::Label monitorHeader { {}, "MONITOR (hear yourself)" };
    juce::ToggleButton monitorToggle { "Monitoring on" };
    juce::Slider monitorGain;
    LevelMeter outputMeter;
    juce::Label latencyLabel;

    // Virtual mic
    juce::Label vmHeader { {}, "VIRTUAL MIC OUTPUT (use it as the microphone in games / Discord / OBS)" };
    juce::ComboBox vmTargetBox;
    juce::ToggleButton vmMuteToggle { "Mute" };
    juce::Slider vmGain, vmSafety;
    juce::Label vmSafetyLabel { {}, "Safety buffer" }, vmStatusLabel, vmStatsLabel;
    juce::Colour vmStatusColour { juce::Colours::grey };

    juce::StringArray vmTargetIds;   // index i <-> combo item id i + 1
    juce::String currentVmTarget;

    std::map<juce::AudioProcessor*, std::unique_ptr<PluginWindow>> editors;
    std::unique_ptr<juce::FileChooser> fileChooser;
    int statusCountdown = 0;
    int autoRetryCountdown = 30 * 5;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace vb
