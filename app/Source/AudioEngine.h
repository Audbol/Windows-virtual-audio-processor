#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginChain.h"
#include "VirtualMicOutput.h"

namespace vb
{

/**
    The real-time path:

        interface input ──► input routing/gain ──► VST chain ──┬─► monitor out (same device, optional)
                                                               └─► virtual mic (driver ring / fallback device)

    Everything lives in one device callback, so monitoring latency is just the
    interface's own in+out buffers (use ASIO or WASAPI exclusive / low-latency for
    the smallest buffers), and nothing in the path allocates or locks for long.
*/
class AudioEngine final : public juce::AudioIODeviceCallback
{
public:
    enum class InputMode
    {
        monoFirst = 0,  // first enabled input channel (e.g. "Input 1" of an interface)
        monoSecond,     // second enabled input channel
        monoSum,        // average of all enabled input channels
        stereo          // first two enabled channels as L/R
    };

    AudioEngine();
    ~AudioEngine() override;

    juce::AudioDeviceManager& getDeviceManager()     { return deviceManager; }
    juce::AudioPluginFormatManager& getFormats()     { return formatManager; }
    juce::KnownPluginList& getKnownPlugins()         { return knownPlugins; }
    PluginChain& getChain()                          { return chain; }
    VirtualMicOutput& getVirtualMic()                { return virtualMic; }

    /** Opens the audio device (restoring saved state if any) and starts processing. */
    void start (const juce::XmlElement* savedDeviceState);
    void shutdown();

    // Parameters (thread-safe)
    void setInputMode (InputMode m)          { inputMode = (int) m; }
    InputMode getInputMode() const           { return (InputMode) inputMode.load(); }
    void setInputGainDb (float db)           { inputGain = juce::Decibels::decibelsToGain (db); }
    void setMonitorEnabled (bool on)         { monitorEnabled = on; }
    bool isMonitorEnabled() const            { return monitorEnabled.load(); }
    void setMonitorGainDb (float db)         { monitorGain = juce::Decibels::decibelsToGain (db, -60.0f); }

    // Meters: peak level since last call (message thread)
    float popInputPeak()                     { return inputPeak.exchange (0.0f); }
    float popOutputPeak()                    { return outputPeak.exchange (0.0f); }

    struct LatencyInfo
    {
        double sampleRate = 0;
        int bufferSize = 0;
        double inputMs = 0, outputMs = 0, pluginMs = 0;
        double monitorRoundTripMs = 0;
    };
    LatencyInfo getLatencyInfo();

private:
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    juce::AudioDeviceManager deviceManager;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;

    PluginChain chain;
    VirtualMicOutput virtualMic;

    juce::AudioBuffer<float> work;

    std::atomic<int> inputMode { (int) InputMode::monoFirst };
    std::atomic<float> inputGain { 1.0f };
    std::atomic<bool> monitorEnabled { true };
    std::atomic<float> monitorGain { 1.0f };
    std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };
};

} // namespace vb
