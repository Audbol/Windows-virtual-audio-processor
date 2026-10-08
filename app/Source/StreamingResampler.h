#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace vb
{

/**
    Tiny streaming resampler (4-point Hermite) with a continuously variable ratio.

    Used to convert from the interface's sample rate to the virtual mic's 48 kHz
    *and* to absorb clock drift between the two (the ratio is nudged a few ppm by
    the drift controller). Its group delay is ~2 input samples, which is why it was
    chosen over a windowed-sinc design: latency matters more than ultimate
    stop-band rejection for live voice.

    Real-time safe after prepare().
*/
class StreamingResampler
{
public:
    void prepare (int numChannelsToUse, int maxInputBlock)
    {
        numChannels = numChannelsToUse;
        history.setSize (numChannels, maxInputBlock + kKeep, false, true, false);
        reset();
    }

    void reset()
    {
        history.clear();
        position = 1.0;
    }

    /** Maximum number of output samples process() may produce for numInput samples. */
    static int maxOutputFor (int numInput, double minStep)
    {
        return (int) std::ceil ((numInput + kKeep) / juce::jmax (1.0e-3, minStep)) + 2;
    }

    /** Consumes all numInput samples and writes as many output samples as are
        available at 'step' input samples per output sample. Returns the count. */
    int process (const float* const* input, int numInput, double step, float* const* output, int maxOutput)
    {
        jassert (numInput + kKeep <= history.getNumSamples());
        numInput = juce::jmin (numInput, history.getNumSamples() - kKeep);

        for (int ch = 0; ch < numChannels; ++ch)
            history.copyFrom (ch, kKeep, input[ch], numInput);

        const int total = kKeep + numInput;
        int produced = 0;

        while (produced < maxOutput)
        {
            const int i = (int) position;
            if (i + 2 > total - 1)
                break;

            const float t = (float) (position - i);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float* x = history.getReadPointer (ch);
                output[ch][produced] = hermite (x[i - 1], x[i], x[i + 1], x[i + 2], t);
            }

            ++produced;
            position += step;
        }

        // Keep the last kKeep samples as history for the next block.
        const int newStart = total - kKeep;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* x = history.getWritePointer (ch);
            for (int k = 0; k < kKeep; ++k)
                x[k] = x[newStart + k];
        }
        position -= newStart;

        return produced;
    }

private:
    static constexpr int kKeep = 3;

    static float hermite (float xm1, float x0, float x1, float x2, float t) noexcept
    {
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }

    int numChannels = 2;
    juce::AudioBuffer<float> history;
    double position = 1.0;
};

} // namespace vb
