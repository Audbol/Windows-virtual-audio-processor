#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include "StreamingResampler.h"

namespace vb
{

//==============================================================================
/** Somewhere the processed voice is sent so other apps can record it. */
class VirtualMicBackend
{
public:
    enum class State
    {
        unavailable,    // driver not installed / device failed to open
        busy,           // another VocalBridge instance owns the virtual mic
        idle,           // connected, but nobody is recording from it right now
        streaming       // connected and an app is actively recording
    };

    struct Status
    {
        State state = State::unavailable;
        juce::String text;
        int underruns = 0;
        int resyncs = 0;
        double bufferedMs = 0.0;
    };

    virtual ~VirtualMicBackend() = default;

    /** Sample rate the backend consumes. */
    virtual double getSampleRate() const = 0;

    /** Frames the backend pulls per period (used to size the safety buffer). */
    virtual int getConsumerBlockFrames() const = 0;

    /** True while something is actively consuming the audio (drift control runs). */
    virtual bool isConsuming() const = 0;

    /** Frames written but not yet consumed. */
    virtual juce::int64 getFillFrames() const = 0;

    /** Where the consumer re-syncs to after a glitch (frames behind the writer). */
    virtual void setTargetFillFrames (int frames) = 0;

    /** Audio thread: push stereo frames. Must be wait-free. */
    virtual void write (const float* left, const float* right, int numFrames) = 0;

    /** Message thread: snapshot for the UI. */
    virtual Status getStatus() const = 0;
};

#if JUCE_WINDOWS
//==============================================================================
/** Feeds the VocalBridge kernel driver's virtual microphone through a shared
    ring buffer (see shared/VocalBridgeShared.h). Zero-copy into the driver:
    the driver reads the ring directly from its 1 ms timer. */
class DriverBackend final : public VirtualMicBackend
{
public:
    DriverBackend();
    ~DriverBackend() override;

    double getSampleRate() const override;
    int getConsumerBlockFrames() const override;
    bool isConsuming() const override;
    juce::int64 getFillFrames() const override;
    void setTargetFillFrames (int frames) override;
    void write (const float* left, const float* right, int numFrames) override;
    Status getStatus() const override;

private:
    class LinkThread;
    struct Ring;
    std::unique_ptr<Ring> ring;
    std::unique_ptr<LinkThread> link;
};
#endif

//==============================================================================
/** Fallback: plays the processed voice into any output device, e.g. a signed
    third-party virtual cable ("CABLE Input (VB-Audio Virtual Cable)").
    Useful on PCs where test-signed drivers are not an option. */
class DeviceBackend final : public VirtualMicBackend,
                            private juce::AudioIODeviceCallback
{
public:
    /** typeName/deviceName come from the AudioDeviceManager's device types. */
    DeviceBackend (juce::AudioDeviceManager& manager, const juce::String& typeName, const juce::String& deviceName);
    ~DeviceBackend() override;

    double getSampleRate() const override;
    int getConsumerBlockFrames() const override;
    bool isConsuming() const override;
    juce::int64 getFillFrames() const override;
    void setTargetFillFrames (int frames) override;
    void write (const float* left, const float* right, int numFrames) override;
    Status getStatus() const override;

private:
    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const*, int, int,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}

    static constexpr int kFifoFrames = 32768;

    std::unique_ptr<juce::AudioIODevice> device;
    juce::String openError, deviceLabel;
    double sampleRate = 48000.0;
    int blockFrames = 512;

    juce::AbstractFifo fifo { kFifoFrames };
    juce::AudioBuffer<float> fifoData { 2, kFifoFrames };
    std::atomic<int> targetFill { 512 };
    std::atomic<bool> running { false };
    std::atomic<int> underruns { 0 }, resyncs { 0 };
    bool starved = true;
};

//==============================================================================
/**
    Converts the processed voice to the backend's rate, keeps the amount of
    buffered audio at a small, constant target by gently trimming the resampling
    ratio (clock-drift compensation), and hands it to the backend.

    Latency budget into the virtual mic ≈ one app buffer + "safety" ms.
*/
class VirtualMicOutput
{
public:
    VirtualMicOutput();
    ~VirtualMicOutput();

    /** Audio thread is stopped or this is called from aboutToStart. */
    void prepare (double deviceSampleRate, int maxBlockSize);

    /** Audio thread. */
    void process (const float* left, const float* right, int numSamples);

    /** Message thread: swap the backend (nullptr = off). */
    void setBackend (std::unique_ptr<VirtualMicBackend> newBackend);

    void setGain (float linear)            { gain.store (linear); }
    void setMuted (bool shouldMute)        { muted.store (shouldMute); }
    void setSafetyMs (double ms)           { safetyMs.store (ms); }

    bool hasBackend() const;
    VirtualMicBackend::Status getStatus() const;

    /** Estimated audio latency added between app output and virtual mic, ms. */
    double getEstimatedLatencyMs() const   { return estimatedLatencyMs.load(); }

private:
    void allocateLocked();

    static constexpr double kMaxTrim = 0.003;   // max ±0.3 % ratio correction

    juce::CriticalSection lock;           // held by the audio thread while processing
    std::unique_ptr<VirtualMicBackend> backend;

    StreamingResampler resampler;
    juce::AudioBuffer<float> scratchIn, scratchOut;
    double deviceRate = 48000.0;
    int deviceBlock = 512;
    double smoothedFill = 0.0;
    double ratioTrim = 0.0;
    bool wasConsuming = false;
    int lastTargetSent = -1;

    std::atomic<float> gain { 1.0f };
    std::atomic<bool> muted { false };
    std::atomic<double> safetyMs { 3.0 };
    std::atomic<double> estimatedLatencyMs { 0.0 };
};

} // namespace vb
