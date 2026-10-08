// Offline simulation tests for the low-latency virtual mic path:
//  - StreamingResampler: rate conversion quality + block-boundary continuity
//  - VirtualMicOutput: drift compensation keeps the queue at the setpoint with
//    no underruns while the "virtual mic" consumes on an independent clock.

#include <juce_audio_basics/juce_audio_basics.h>
#include "../Source/StreamingResampler.h"
#include "../Source/VirtualMicOutput.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
int failures = 0;

void check (bool ok, const char* what)
{
    std::printf ("[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (! ok) ++failures;
}

//==============================================================================
/** Consumer that behaves like the driver: pulls 1 ms worth every tick on its
    own (drifting) clock, with VB driver-style underrun / resync handling. */
class MockDriver final : public vb::VirtualMicBackend
{
public:
    double getSampleRate() const override         { return 48000.0; }
    int getConsumerBlockFrames() const override   { return 48; }
    bool isConsuming() const override             { return true; }
    juce::int64 getFillFrames() const override    { return writeFrame - readFrame; }
    void setTargetFillFrames (int f) override     { target = f; }
    Status getStatus() const override             { return {}; }

    void write (const float* l, const float*, int n) override
    {
        for (int i = 0; i < n; ++i)
            ring[(size_t) ((writeFrame + i) & (kCap - 1))] = l[i];
        writeFrame += n;
    }

    void consume (int frames, std::vector<float>& out)
    {
        juce::int64 fill = writeFrame - readFrame;
        if (needResync || fill < 0 || fill > kCap - frames)
        {
            readFrame = writeFrame - target;
            needResync = false;
            starved = false;
            fill = target;
            ++resyncs;
        }
        if (starved && fill >= target)
            starved = false;

        int copied = 0;
        if (! starved)
        {
            copied = (int) std::min<juce::int64> (fill, frames);
            if (copied < frames) { starved = true; ++underruns; }
        }
        for (int i = 0; i < copied; ++i)
            out.push_back (ring[(size_t) ((readFrame + i) & (kCap - 1))]);
        for (int i = copied; i < frames; ++i)
            out.push_back (0.0f);
        readFrame += copied;
    }

    static constexpr juce::int64 kCap = 8192;
    std::vector<float> ring = std::vector<float> ((size_t) kCap);
    juce::int64 writeFrame = 0, readFrame = 0;
    int target = 256, underruns = 0, resyncs = -1;  // first resync is the initial sync
    bool needResync = true, starved = false;
};

//==============================================================================
void testResampler()
{
    const double inRate = 44100.0, outRate = 48000.0, freq = 1000.0;
    vb::StreamingResampler rs;
    rs.prepare (2, 512);

    std::vector<float> out;
    std::vector<float> outBuf ((size_t) vb::StreamingResampler::maxOutputFor (512, inRate / outRate));
    std::vector<float> outBuf2 (outBuf.size());
    std::vector<float> in (512), in2 (512);
    juce::int64 n = 0;
    juce::Random rng (1);

    for (int block = 0; block < 400; ++block)
    {
        const int size = 64 + rng.nextInt (448);   // ragged block sizes
        for (int i = 0; i < size; ++i, ++n)
            in[(size_t) i] = in2[(size_t) i] = (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * (double) n / inRate);

        const float* ins[] = { in.data(), in2.data() };
        float* outs[] = { outBuf.data(), outBuf2.data() };
        const int produced = rs.process (ins, size, inRate / outRate, outs, (int) outBuf.size());
        out.insert (out.end(), outBuf.begin(), outBuf.begin() + produced);
    }

    const double expectedCount = (double) n * outRate / inRate;
    check (std::abs ((double) out.size() - expectedCount) < 5.0, "resampler output count matches rate ratio");

    // Compare against an ideal sine (allowing for the ~2-sample group delay) after warm-up.
    double errSq = 0, sigSq = 0, bestErr = 1e9;
    for (double delay = 0.0; delay <= 4.0; delay += 0.01)
    {
        errSq = sigSq = 0;
        for (size_t i = 1000; i < out.size() - 10; ++i)
        {
            const double ideal = std::sin (2.0 * juce::MathConstants<double>::pi * freq * ((double) i - delay) / outRate);
            errSq += (out[i] - ideal) * (out[i] - ideal);
            sigSq += ideal * ideal;
        }
        bestErr = std::min (bestErr, errSq / sigSq);
    }
    const double snrDb = -10.0 * std::log10 (bestErr);
    std::printf ("       resampler SNR @1 kHz 44.1->48 kHz: %.1f dB\n", snrDb);
    check (snrDb > 60.0, "resampler SNR > 60 dB for voice-band sine (continuous across ragged blocks)");
}

//==============================================================================
void testDriftCompensation (double deviceRate, int deviceBlock, double ppm)
{
    vb::VirtualMicOutput vm;
    auto mockPtr = std::make_unique<MockDriver>();
    auto* mock = mockPtr.get();
    vm.setBackend (std::move (mockPtr));
    vm.setSafetyMs (2.0);
    vm.prepare (deviceRate, deviceBlock);

    // Device clock runs 'ppm' fast/slow relative to the consumer (driver) clock.
    const double deviceSecondsPerBlock = deviceBlock / (deviceRate * (1.0 + ppm * 1e-6));
    const double tickSeconds = 0.001;

    std::vector<float> l ((size_t) deviceBlock), r ((size_t) deviceBlock);
    std::vector<float> captured;
    double tDevice = 0.0, tTick = 0.0;
    juce::int64 phase = 0;
    const double simSeconds = 120.0;
    double maxFillLate = 0, minFillLate = 1e9;
    int underrunsAtSettle = 0;

    while (tDevice < simSeconds)
    {
        // Interleave producer blocks and 1 ms consumer ticks in time order.
        if (tDevice <= tTick)
        {
            for (int i = 0; i < deviceBlock; ++i, ++phase)
                l[(size_t) i] = r[(size_t) i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * (double) phase / deviceRate);
            vm.process (l.data(), r.data(), deviceBlock);
            tDevice += deviceSecondsPerBlock;
        }
        else
        {
            mock->consume (48, captured);
            tTick += tickSeconds;

            if (tTick > 20.0 && underrunsAtSettle == 0)
                underrunsAtSettle = mock->underruns + 1;   // +1 so 0 underruns is recorded too

            if (tTick > 20.0)
            {
                const double fill = (double) mock->getFillFrames();
                maxFillLate = std::max (maxFillLate, fill);
                minFillLate = std::min (minFillLate, fill);
            }
        }
    }

    const int lateUnderruns = mock->underruns - (underrunsAtSettle - 1);
    char label[256];
    std::snprintf (label, sizeof (label),
                   "drift %+.0f ppm, %.1f kHz, %d-sample blocks: no underruns/resyncs after settling (fill %.0f..%.0f frames, ~%.2f ms added)",
                   ppm, deviceRate / 1000.0, deviceBlock, minFillLate, maxFillLate, vm.getEstimatedLatencyMs());
    check (lateUnderruns == 0 && mock->resyncs == 0 && maxFillLate < 48 * 12, label);
}
} // namespace

int main()
{
    testResampler();

    testDriftCompensation (48000.0, 128, +150.0);
    testDriftCompensation (48000.0, 128, -150.0);
    testDriftCompensation (48000.0, 64, +50.0);
    testDriftCompensation (44100.0, 128, -200.0);
    testDriftCompensation (96000.0, 256, +200.0);

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
