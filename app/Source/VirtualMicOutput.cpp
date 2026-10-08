#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <winioctl.h>
 #include "VocalBridgeShared.h"
#endif

#include "VirtualMicOutput.h"
#include <atomic>
#include <cmath>

namespace vb
{

#if JUCE_WINDOWS
//==============================================================================
// DriverBackend
//==============================================================================
struct DriverBackend::Ring
{
    static constexpr juce::uint32 kCapacityFrames = 8192;   // ~170 ms, power of two

    Ring()
    {
        bytes = VOCALBRIDGE_RING_HEADER_SIZE + (size_t) kCapacityFrames * VOCALBRIDGE_BYTES_PER_FRAME;
        memory = VirtualAlloc (nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        jassert (memory != nullptr);

        if (memory == nullptr)
            return;

        // Keep the ring resident so the audio thread never page-faults on it.
        VirtualLock (memory, bytes);

        header = static_cast<VOCALBRIDGE_RING_HEADER*> (memory);
        header->Magic            = VOCALBRIDGE_RING_MAGIC;
        header->Version          = VOCALBRIDGE_PROTOCOL_VERSION;
        header->HeaderSize       = VOCALBRIDGE_RING_HEADER_SIZE;
        header->SampleRate       = VOCALBRIDGE_SAMPLE_RATE;
        header->Channels         = VOCALBRIDGE_CHANNELS;
        header->CapacityFrames   = kCapacityFrames;
        header->TargetFillFrames = 256;

        samples = reinterpret_cast<juce::int32*> (static_cast<char*> (memory) + VOCALBRIDGE_RING_HEADER_SIZE);
    }

    ~Ring()
    {
        if (memory != nullptr)
        {
            VirtualUnlock (memory, bytes);
            VirtualFree (memory, 0, MEM_RELEASE);
        }
    }

    static std::atomic_ref<long long> ref (volatile long long& v) noexcept { return std::atomic_ref<long long> (const_cast<long long&> (v)); }
    static std::atomic_ref<long>      ref (volatile long& v) noexcept      { return std::atomic_ref<long> (const_cast<long&> (v)); }

    void* memory = nullptr;
    size_t bytes = 0;
    VOCALBRIDGE_RING_HEADER* header = nullptr;
    juce::int32* samples = nullptr;
    long long writeFrame = 0;               // producer-private copy of header->WriteFrame
};

//==============================================================================
class DriverBackend::LinkThread final : public juce::Thread
{
public:
    explicit LinkThread (Ring& r) : juce::Thread ("VocalBridge driver link"), ring (r)
    {
        stopEvent = CreateEventW (nullptr, TRUE, FALSE, nullptr);
        startThread();
    }

    ~LinkThread() override
    {
        signalThreadShouldExit();
        SetEvent (stopEvent);
        stopThread (5000);
        CloseHandle (stopEvent);
    }

    std::atomic<bool> attached { false };
    std::atomic<VirtualMicBackend::State> state { VirtualMicBackend::State::unavailable };

    juce::String getMessage() const
    {
        const juce::ScopedLock sl (messageLock);
        return message;
    }

private:
    void setStatus (VirtualMicBackend::State s, const juce::String& text)
    {
        state = s;
        const juce::ScopedLock sl (messageLock);
        message = text;
    }

    bool sleepOrStop (DWORD ms) const
    {
        return WaitForSingleObject (stopEvent, ms) == WAIT_OBJECT_0;
    }

    void run() override
    {
        // The attach IOCTL is cancelled by Windows if the issuing thread exits, so
        // this thread lives exactly as long as the link.
        while (! threadShouldExit())
        {
            if (ring.memory == nullptr)
            {
                setStatus (VirtualMicBackend::State::unavailable, "Out of memory");
                return;
            }

            HANDLE h = CreateFileW (VOCALBRIDGE_USER_DEVICE_PATH,
                                    GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);

            if (h == INVALID_HANDLE_VALUE)
            {
                setStatus (VirtualMicBackend::State::unavailable, "VocalBridge driver not installed");
                if (sleepOrStop (1000)) break;
                continue;
            }

            OVERLAPPED ov {};
            ov.hEvent = CreateEventW (nullptr, TRUE, FALSE, nullptr);

            const BOOL ok = DeviceIoControl (h, IOCTL_VOCALBRIDGE_ATTACH_RING,
                                             nullptr, 0,
                                             ring.memory, (DWORD) ring.bytes,
                                             nullptr, &ov);
            DWORD err = ok ? ERROR_SUCCESS : GetLastError();

            if (! ok && err == ERROR_IO_PENDING)
            {
                attached = true;
                setStatus (VirtualMicBackend::State::idle, "Connected to VocalBridge Virtual Mic");

                HANDLE events[] = { ov.hEvent, stopEvent };
                if (WaitForMultipleObjects (2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
                    CancelIoEx (h, &ov);

                DWORD transferred = 0;
                err = GetOverlappedResult (h, &ov, &transferred, TRUE) ? ERROR_SUCCESS : GetLastError();
                attached = false;
            }

            CloseHandle (ov.hEvent);
            CloseHandle (h);

            if (threadShouldExit())
                break;

            if (err == ERROR_BUSY)
                setStatus (VirtualMicBackend::State::busy, "Virtual mic is in use by another VocalBridge window");
            else if (err == ERROR_REVISION_MISMATCH)
                setStatus (VirtualMicBackend::State::unavailable, "Driver/app version mismatch - reinstall VocalBridge");
            else
                setStatus (VirtualMicBackend::State::unavailable, "Driver link lost (error " + juce::String ((int) err) + "), retrying...");

            if (sleepOrStop (1000))
                break;
        }
    }

    Ring& ring;
    HANDLE stopEvent = nullptr;
    juce::CriticalSection messageLock;
    juce::String message { "Connecting..." };
};

//==============================================================================
DriverBackend::DriverBackend()
    : ring (std::make_unique<Ring>())
{
    link = std::make_unique<LinkThread> (*ring);
}

DriverBackend::~DriverBackend()
{
    link.reset();   // detaches the ring before its memory is freed
    ring.reset();
}

double DriverBackend::getSampleRate() const    { return (double) VOCALBRIDGE_SAMPLE_RATE; }
int DriverBackend::getConsumerBlockFrames() const { return (int) VOCALBRIDGE_SAMPLE_RATE / 1000; }  // driver ticks every 1 ms

bool DriverBackend::isConsuming() const
{
    return ring->header != nullptr && link->attached.load()
        && Ring::ref (ring->header->CaptureActive).load (std::memory_order_relaxed) != 0;
}

juce::int64 DriverBackend::getFillFrames() const
{
    if (ring->header == nullptr)
        return 0;

    return ring->writeFrame - Ring::ref (ring->header->ReadFrame).load (std::memory_order_acquire);
}

void DriverBackend::setTargetFillFrames (int frames)
{
    if (ring->header != nullptr)
        Ring::ref (ring->header->TargetFillFrames).store ((long) frames, std::memory_order_relaxed);
}

void DriverBackend::write (const float* left, const float* right, int numFrames)
{
    if (ring->header == nullptr)
        return;

    constexpr juce::uint32 mask = Ring::kCapacityFrames - 1;
    auto* dst = ring->samples;
    long long w = ring->writeFrame;

    auto toPcm32 = [] (float x) noexcept
    {
        x = juce::jlimit (-1.0f, 1.0f, x);
        return (juce::int32) (x * 2147483520.0f);   // largest float below 2^31
    };

    for (int i = 0; i < numFrames; ++i)
    {
        const auto idx = (juce::uint32) (w + i) & mask;
        dst[idx * 2]     = toPcm32 (left[i]);
        dst[idx * 2 + 1] = toPcm32 (right[i]);
    }

    w += numFrames;
    ring->writeFrame = w;
    Ring::ref (ring->header->WriteFrame).store (w, std::memory_order_release);
}

VirtualMicBackend::Status DriverBackend::getStatus() const
{
    Status s;
    s.state = link->state.load();
    s.text = link->getMessage();

    if (s.state == State::idle && isConsuming())
    {
        s.state = State::streaming;
        s.text = "Streaming to VocalBridge Virtual Mic";
    }

    if (ring->header != nullptr)
    {
        s.underruns = (int) Ring::ref (ring->header->UnderrunCount).load (std::memory_order_relaxed);
        s.resyncs   = (int) Ring::ref (ring->header->ResyncCount).load (std::memory_order_relaxed);
        s.bufferedMs = (double) getFillFrames() * 1000.0 / getSampleRate();
    }
    return s;
}
#endif // JUCE_WINDOWS

//==============================================================================
// DeviceBackend
//==============================================================================
DeviceBackend::DeviceBackend (juce::AudioDeviceManager& manager, const juce::String& typeName, const juce::String& deviceName)
    : deviceLabel (deviceName)
{
    fifoData.clear();

    juce::AudioIODeviceType* type = nullptr;
    for (auto* t : manager.getAvailableDeviceTypes())
        if (t->getTypeName() == typeName)
            type = t;

    if (type == nullptr)
    {
        openError = "Device type not available: " + typeName;
        return;
    }

    type->scanForDevices();
    device.reset (type->createDevice (deviceName, {}));

    if (device == nullptr)
    {
        openError = "Could not create device: " + deviceName;
        return;
    }

    const auto rates = device->getAvailableSampleRates();
    double rate = rates.contains (48000.0) ? 48000.0 : (rates.isEmpty() ? 48000.0 : rates[0]);

    // Smallest buffer >= 96 samples keeps the hand-off latency low without
    // starving the device.
    int buffer = device->getDefaultBufferSize();
    for (auto b : device->getAvailableBufferSizes())
    {
        if (b >= 96)
        {
            buffer = b;
            break;
        }
    }

    juce::BigInteger outputs;
    outputs.setRange (0, 2, true);

    openError = device->open ({}, outputs, rate, buffer);
    if (openError.isNotEmpty())
    {
        device.reset();
        return;
    }

    sampleRate = device->getCurrentSampleRate();
    blockFrames = device->getCurrentBufferSizeSamples();
    targetFill = blockFrames * 2;
    device->start (this);
    running = true;
}

DeviceBackend::~DeviceBackend()
{
    running = false;
    if (device != nullptr)
    {
        device->stop();
        device->close();
    }
}

double DeviceBackend::getSampleRate() const          { return sampleRate; }
int DeviceBackend::getConsumerBlockFrames() const    { return blockFrames; }
bool DeviceBackend::isConsuming() const              { return running.load() && device != nullptr && device->isPlaying(); }
juce::int64 DeviceBackend::getFillFrames() const     { return fifo.getNumReady(); }
void DeviceBackend::setTargetFillFrames (int frames) { targetFill = juce::jlimit (16, kFifoFrames / 2, frames); }

void DeviceBackend::write (const float* left, const float* right, int numFrames)
{
    int start1, size1, start2, size2;
    fifo.prepareToWrite (numFrames, start1, size1, start2, size2);   // drops what doesn't fit

    if (size1 > 0)
    {
        fifoData.copyFrom (0, start1, left, size1);
        fifoData.copyFrom (1, start1, right, size1);
    }
    if (size2 > 0)
    {
        fifoData.copyFrom (0, start2, left + size1, size2);
        fifoData.copyFrom (1, start2, right + size1, size2);
    }

    fifo.finishedWrite (size1 + size2);
}

void DeviceBackend::audioDeviceIOCallbackWithContext (const float* const*, int,
                                                      float* const* outputs, int numOutputs, int numSamples,
                                                      const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOutputs; ++ch)
        if (outputs[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputs[ch], numSamples);

    if (numOutputs <= 0 || outputs[0] == nullptr)
        return;

    const int target = targetFill.load();
    int available = fifo.getNumReady();

    // Too far behind the producer (e.g. after a stall): skip ahead to the target.
    if (available > target + (int) (sampleRate * 0.05) + numSamples)
    {
        fifo.finishedRead (available - target);
        available = target;
        ++resyncs;
    }

    // After running dry, wait until we're back at the target before resuming.
    if (starved && available >= target)
        starved = false;

    int toRead = 0;
    if (! starved)
    {
        toRead = juce::jmin (available, numSamples);
        if (toRead < numSamples)
        {
            starved = true;
            ++underruns;
        }
    }

    int start1, size1, start2, size2;
    fifo.prepareToRead (toRead, start1, size1, start2, size2);

    float* left = outputs[0];
    float* right = (numOutputs > 1 && outputs[1] != nullptr) ? outputs[1] : nullptr;

    auto copyRegion = [&] (int fifoStart, int count, int destOffset)
    {
        if (count <= 0)
            return;

        if (right != nullptr)
        {
            juce::FloatVectorOperations::copy (left + destOffset, fifoData.getReadPointer (0, fifoStart), count);
            juce::FloatVectorOperations::copy (right + destOffset, fifoData.getReadPointer (1, fifoStart), count);
        }
        else
        {
            juce::FloatVectorOperations::copyWithMultiply (left + destOffset, fifoData.getReadPointer (0, fifoStart), 0.5f, count);
            juce::FloatVectorOperations::addWithMultiply (left + destOffset, fifoData.getReadPointer (1, fifoStart), 0.5f, count);
        }
    };

    copyRegion (start1, size1, 0);
    copyRegion (start2, size2, size1);
    fifo.finishedRead (size1 + size2);
}

VirtualMicBackend::Status DeviceBackend::getStatus() const
{
    Status s;
    if (device == nullptr)
    {
        s.state = State::unavailable;
        s.text = openError.isNotEmpty() ? openError : "Device unavailable";
        return s;
    }

    s.state = isConsuming() ? State::streaming : State::idle;
    s.text = "Playing into \"" + deviceLabel + "\" @ " + juce::String (sampleRate / 1000.0, 1) + " kHz";
    s.underruns = underruns.load();
    s.resyncs = resyncs.load();
    s.bufferedMs = (double) fifo.getNumReady() * 1000.0 / sampleRate;
    return s;
}

//==============================================================================
// VirtualMicOutput
//==============================================================================
VirtualMicOutput::VirtualMicOutput() = default;
VirtualMicOutput::~VirtualMicOutput() = default;

void VirtualMicOutput::prepare (double deviceSampleRate, int maxBlockSize)
{
    const juce::ScopedLock sl (lock);
    deviceRate = deviceSampleRate;
    deviceBlock = juce::jmax (1, maxBlockSize);
    allocateLocked();
}

void VirtualMicOutput::allocateLocked()
{
    scratchIn.setSize (2, deviceBlock, false, true, false);

    // Worst case output count: backend rate up to 4x the device rate.
    const double backendRate = backend != nullptr ? backend->getSampleRate() : 48000.0;
    const double minStep = (deviceRate / juce::jmax (backendRate, deviceRate / 4.0)) * (1.0 - kMaxTrim);
    scratchOut.setSize (2, StreamingResampler::maxOutputFor (deviceBlock, minStep), false, true, false);

    resampler.prepare (2, deviceBlock);
    wasConsuming = false;
    ratioTrim = 0.0;
    lastTargetSent = -1;
}

void VirtualMicOutput::setBackend (std::unique_ptr<VirtualMicBackend> newBackend)
{
    std::unique_ptr<VirtualMicBackend> old;
    {
        const juce::ScopedLock sl (lock);
        old = std::move (backend);
        backend = std::move (newBackend);
        allocateLocked();
    }
    // 'old' is destroyed here, outside the lock (may block while it shuts down).
}

bool VirtualMicOutput::hasBackend() const
{
    return backend != nullptr;   // only mutated on the message thread
}

VirtualMicBackend::Status VirtualMicOutput::getStatus() const
{
    if (backend == nullptr)
    {
        VirtualMicBackend::Status s;
        s.text = "Off";
        return s;
    }
    return backend->getStatus();
}

void VirtualMicOutput::process (const float* left, const float* right, int numSamples)
{
    const juce::ScopedTryLock sl (lock);
    if (! sl.isLocked() || backend == nullptr)
        return;

    // Very defensive: never touch more than we allocated for.
    numSamples = juce::jmin (numSamples, scratchIn.getNumSamples());

    const float g = muted.load() ? 0.0f : gain.load();
    scratchIn.copyFrom (0, 0, left, numSamples, g);
    scratchIn.copyFrom (1, 0, right, numSamples, g);

    const double backendRate = backend->getSampleRate();

    // Setpoint for "frames still queued just before we write":
    // one consumer period + user safety margin.
    const int setpoint = backend->getConsumerBlockFrames()
                       + (int) std::lround (safetyMs.load() * backendRate / 1000.0);
    const int appBlockAtBackend = (int) std::lround (numSamples * backendRate / deviceRate);
    const int resyncTarget = setpoint + appBlockAtBackend;

    if (resyncTarget != lastTargetSent)
    {
        backend->setTargetFillFrames (resyncTarget);
        lastTargetSent = resyncTarget;
    }

    // Clock-drift compensation: nudge the resampling ratio (at most ±0.3 %, which
    // is inaudible) so the queued amount converges on the setpoint.
    const bool consuming = backend->isConsuming();
    if (consuming)
    {
        const double fill = (double) backend->getFillFrames();

        if (! wasConsuming)
            smoothedFill = fill;

        const double alpha = 1.0 - std::exp (-(double) numSamples / (deviceRate * 0.25));
        smoothedFill += alpha * (fill - smoothedFill);

        const double error = smoothedFill - (double) setpoint;
        ratioTrim = juce::jlimit (-kMaxTrim, kMaxTrim, error * 1.0e-5);

        estimatedLatencyMs.store ((smoothedFill + appBlockAtBackend * 0.5) * 1000.0 / backendRate);
    }
    else
    {
        ratioTrim = 0.0;
        estimatedLatencyMs.store (0.0);
    }
    wasConsuming = consuming;

    const double step = (deviceRate / backendRate) * (1.0 + ratioTrim);

    const float* in[] = { scratchIn.getReadPointer (0), scratchIn.getReadPointer (1) };
    float* out[] = { scratchOut.getWritePointer (0), scratchOut.getWritePointer (1) };

    const int produced = resampler.process (in, numSamples, step, out, scratchOut.getNumSamples());
    backend->write (out[0], out[1], produced);
}

} // namespace vb
