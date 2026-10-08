#include "AudioEngine.h"

namespace vb
{

AudioEngine::AudioEngine()
{
    formatManager.addDefaultFormats();
}

AudioEngine::~AudioEngine()
{
    shutdown();
}

//==============================================================================
static juce::String pickLowLatencyDeviceType (juce::AudioDeviceManager& dm)
{
    // Preference order for live monitoring: ASIO (if built with the SDK and a
    // driver is present) > WASAPI low-latency shared mode > whatever is default.
    const char* preferred[] = { "ASIO", "Windows Audio (Low Latency Mode)", "Windows Audio", "ALSA", "CoreAudio" };

    for (auto* name : preferred)
    {
        for (auto* type : dm.getAvailableDeviceTypes())
        {
            if (type->getTypeName() == name)
            {
                type->scanForDevices();
                if (type->getDeviceNames (true).size() > 0)
                    return type->getTypeName();
            }
        }
    }
    return {};
}

void AudioEngine::start (const juce::XmlElement* savedDeviceState)
{
    const auto error = deviceManager.initialise (2, 2, savedDeviceState, true);
    if (error.isNotEmpty())
    {
        DBG ("Audio device error: " << error);   // the UI shows "no device open" and offers Audio Settings
    }

    if (savedDeviceState == nullptr)
    {
        const auto typeName = pickLowLatencyDeviceType (deviceManager);
        if (typeName.isNotEmpty() && typeName != deviceManager.getCurrentAudioDeviceType())
            deviceManager.setCurrentAudioDeviceType (typeName, true);

        // Small buffer by default: smallest available >= 64 samples.
        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            auto setup = deviceManager.getAudioDeviceSetup();
            for (auto size : device->getAvailableBufferSizes())
            {
                if (size >= 64)
                {
                    setup.bufferSize = size;
                    break;
                }
            }
            deviceManager.setAudioDeviceSetup (setup, true);
        }
    }

    deviceManager.addAudioCallback (this);
}

void AudioEngine::shutdown()
{
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
    virtualMic.setBackend (nullptr);
    chain.clear();
}

//==============================================================================
void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double rate = device->getCurrentSampleRate();
    const int block = device->getCurrentBufferSizeSamples();

    work.setSize (2, block, false, true, false);
    chain.prepare (rate, block);
    virtualMic.prepare (rate, block);
}

void AudioEngine::audioDeviceStopped()
{
    chain.release();
}

static void updatePeak (std::atomic<float>& peak, float value) noexcept
{
    if (value > peak.load (std::memory_order_relaxed))
        peak.store (value, std::memory_order_relaxed);
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                                    float* const* outputs, int numOutputs,
                                                    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;

    if (numSamples > work.getNumSamples())
        work.setSize (2, numSamples, false, false, true);   // only if a driver lies about its block size

    float* left = work.getWritePointer (0);
    float* right = work.getWritePointer (1);

    auto input = [&] (int index) -> const float*
    {
        return juce::isPositiveAndBelow (index, numInputs) ? inputs[index] : nullptr;
    };

    auto copyOrClear = [numSamples] (float* dst, const float* src)
    {
        if (src != nullptr) juce::FloatVectorOperations::copy (dst, src, numSamples);
        else                juce::FloatVectorOperations::clear (dst, numSamples);
    };

    // ---- input routing ----
    switch ((InputMode) inputMode.load (std::memory_order_relaxed))
    {
        case InputMode::monoFirst:
            copyOrClear (left, input (0));
            juce::FloatVectorOperations::copy (right, left, numSamples);
            break;

        case InputMode::monoSecond:
            copyOrClear (left, input (1) != nullptr ? input (1) : input (0));
            juce::FloatVectorOperations::copy (right, left, numSamples);
            break;

        case InputMode::monoSum:
        {
            juce::FloatVectorOperations::clear (left, numSamples);
            int used = 0;
            for (int ch = 0; ch < numInputs; ++ch)
            {
                if (inputs[ch] != nullptr)
                {
                    juce::FloatVectorOperations::add (left, inputs[ch], numSamples);
                    ++used;
                }
            }
            if (used > 1)
                juce::FloatVectorOperations::multiply (left, 1.0f / (float) used, numSamples);
            juce::FloatVectorOperations::copy (right, left, numSamples);
            break;
        }

        case InputMode::stereo:
            copyOrClear (left, input (0));
            copyOrClear (right, input (1) != nullptr ? input (1) : input (0));
            break;
    }

    const float inGain = inputGain.load (std::memory_order_relaxed);
    juce::FloatVectorOperations::multiply (left, inGain, numSamples);
    juce::FloatVectorOperations::multiply (right, inGain, numSamples);

    juce::AudioBuffer<float> block (work.getArrayOfWritePointers(), 2, numSamples);
    updatePeak (inputPeak, block.getMagnitude (0, numSamples));

    // ---- plugins ----
    chain.process (block);

    updatePeak (outputPeak, block.getMagnitude (0, numSamples));

    // ---- virtual mic ----
    virtualMic.process (left, right, numSamples);

    // ---- monitoring (same device => lowest possible monitoring latency) ----
    const bool monitor = monitorEnabled.load (std::memory_order_relaxed);
    const float monGain = monitorGain.load (std::memory_order_relaxed);
    int written = 0;

    for (int ch = 0; ch < numOutputs; ++ch)
    {
        float* out = outputs[ch];
        if (out == nullptr)
            continue;

        if (monitor && written < 2)
        {
            if (numOutputs == 1)
            {
                juce::FloatVectorOperations::copyWithMultiply (out, left, 0.5f * monGain, numSamples);
                juce::FloatVectorOperations::addWithMultiply (out, right, 0.5f * monGain, numSamples);
            }
            else
            {
                juce::FloatVectorOperations::copyWithMultiply (out, written == 0 ? left : right, monGain, numSamples);
            }
            ++written;
        }
        else
        {
            juce::FloatVectorOperations::clear (out, numSamples);
        }
    }
}

//==============================================================================
AudioEngine::LatencyInfo AudioEngine::getLatencyInfo()
{
    LatencyInfo info;

    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        info.sampleRate = device->getCurrentSampleRate();
        info.bufferSize = device->getCurrentBufferSizeSamples();

        if (info.sampleRate > 0)
        {
            const double toMs = 1000.0 / info.sampleRate;
            info.inputMs = device->getInputLatencyInSamples() * toMs;
            info.outputMs = device->getOutputLatencyInSamples() * toMs;
            info.pluginMs = chain.getTotalLatencySamples() * toMs;
            info.monitorRoundTripMs = info.inputMs + info.outputMs + info.pluginMs;
        }
    }

    return info;
}

} // namespace vb
