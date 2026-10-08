#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

#include <juce_audio_utils/juce_audio_utils.h>
#include "AudioEngine.h"
#include "MainComponent.h"

namespace
{
    /** Keep Windows from treating us as a background app: EcoQoS / power throttling
        and timer coalescing add scheduling jitter that shows up as crackles at
        small buffer sizes. */
    void optOutOfPowerThrottling()
    {
       #if defined(_WIN32) && defined(PROCESS_POWER_THROTTLING_CURRENT_VERSION)
        PROCESS_POWER_THROTTLING_STATE state {};
        state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
       #ifdef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
        state.ControlMask |= PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
       #endif
        state.StateMask = 0;   // 0 = throttling OFF for the bits in ControlMask
        SetProcessInformation (GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof (state));
        SetPriorityClass (GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
       #endif
    }
}

//==============================================================================
class VocalBridgeApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return "VocalBridge"; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override             { return false; }

    void initialise (const juce::String&) override
    {
        optOutOfPowerThrottling();

        juce::PropertiesFile::Options options;
        options.applicationName = "VocalBridge";
        options.filenameSuffix = ".settings";
        options.folderName = "VocalBridge";
        options.osxLibrarySubFolder = "Application Support";
        properties.setStorageParameters (options);

        engine = std::make_unique<vb::AudioEngine>();
        auto deviceState = properties.getUserSettings()->getXmlValue ("audioDevice");
        engine->start (deviceState.get());

        mainWindow = std::make_unique<MainWindow> (*engine, properties);
    }

    void shutdown() override
    {
        if (mainWindow != nullptr)
            mainWindow->saveState();

        mainWindow.reset();     // closes plugin editors before plugins are destroyed
        engine.reset();
        properties.closeFiles();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted (const juce::String&) override
    {
        if (mainWindow != nullptr)
            mainWindow->toFront (true);
    }

    //==============================================================================
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow (vb::AudioEngine& engine, juce::ApplicationProperties& props)
            : DocumentWindow ("VocalBridge", juce::Colour (0xff12151a), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            content = new vb::MainComponent (engine, props);
            setContentOwned (content, true);
            setResizable (true, true);
            setResizeLimits (820, 600, 4000, 3000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void saveState()               { content->saveState(); }
        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        vb::MainComponent* content = nullptr;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    juce::ApplicationProperties properties;
    std::unique_ptr<vb::AudioEngine> engine;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (VocalBridgeApplication)
