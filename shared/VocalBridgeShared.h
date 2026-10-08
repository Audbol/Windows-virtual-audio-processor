/*
    VocalBridgeShared.h

    Contract between the VocalBridge kernel driver (virtual microphone) and the
    VocalBridge host application. Included by both kernel-mode and user-mode code,
    so it must only use types available in both <wdm.h> and <windows.h>.

    Data path (lowest-latency design, no extra WASAPI hop):

        [Mic / Interface] -> VocalBridge app (VST chain) --writes--> shared ring
                                                                      |
        Games / Discord / OBS <-- "VocalBridge Virtual Mic" <--reads--+  (driver)

    The app allocates the ring in its own address space and hands it to the driver
    with IOCTL_VOCALBRIDGE_ATTACH_RING (METHOD_OUT_DIRECT). The I/O manager locks the
    pages for as long as that IOCTL stays pending, the driver maps them into system
    space, and every 1 ms timer tick (or whenever the audio engine queries the
    position) copies the newest audio straight into the capture DMA buffer.
    Closing the handle (or CancelIoEx) detaches the ring.

    Ring format is fixed to the virtual mic's native format so the driver never
    converts samples: 48 kHz, 2 channels, 32-bit signed integer PCM, interleaved.
*/

#ifndef VOCALBRIDGE_SHARED_H
#define VOCALBRIDGE_SHARED_H

#define VOCALBRIDGE_KERNEL_DEVICE_NAME  L"\\Device\\VocalBridgeControl"
#define VOCALBRIDGE_KERNEL_SYMLINK_NAME L"\\DosDevices\\Global\\VocalBridge"
#define VOCALBRIDGE_USER_DEVICE_PATH    L"\\\\.\\VocalBridge"

// {6439BE52-12E8-4E67-A9F3-BF40569D952A}
#define VOCALBRIDGE_CONTROL_CLASS_GUID_INIT \
    { 0x6439be52, 0x12e8, 0x4e67, { 0xa9, 0xf3, 0xbf, 0x40, 0x56, 0x9d, 0x95, 0x2a } }

#define VOCALBRIDGE_RING_MAGIC          0x47524256u   // 'VBRG'
#define VOCALBRIDGE_PROTOCOL_VERSION    1u

#define VOCALBRIDGE_SAMPLE_RATE         48000u
#define VOCALBRIDGE_CHANNELS            2u
#define VOCALBRIDGE_BYTES_PER_SAMPLE    4u
#define VOCALBRIDGE_BYTES_PER_FRAME     (VOCALBRIDGE_CHANNELS * VOCALBRIDGE_BYTES_PER_SAMPLE)

// Ring capacity limits (frames, must be a power of two).
#define VOCALBRIDGE_MIN_CAPACITY_FRAMES 1024u      // ~21 ms
#define VOCALBRIDGE_MAX_CAPACITY_FRAMES 65536u     // ~1.36 s

#define VOCALBRIDGE_RING_HEADER_SIZE    256u

#define VOCALBRIDGE_IOCTL_DEVICE_TYPE   0x8000u

// Output buffer = the whole ring (header + samples). Stays pending while attached.
#define IOCTL_VOCALBRIDGE_ATTACH_RING \
    CTL_CODE(VOCALBRIDGE_IOCTL_DEVICE_TYPE, 0x801, METHOD_OUT_DIRECT, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

// Output buffer = VOCALBRIDGE_DRIVER_INFO.
#define IOCTL_VOCALBRIDGE_GET_INFO \
    CTL_CODE(VOCALBRIDGE_IOCTL_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 8)

typedef struct _VOCALBRIDGE_RING_HEADER
{
    // ---- Written once by the app before attaching ----
    unsigned long   Magic;              // VOCALBRIDGE_RING_MAGIC
    unsigned long   Version;            // VOCALBRIDGE_PROTOCOL_VERSION
    unsigned long   HeaderSize;         // VOCALBRIDGE_RING_HEADER_SIZE
    unsigned long   SampleRate;         // VOCALBRIDGE_SAMPLE_RATE
    unsigned long   Channels;           // VOCALBRIDGE_CHANNELS
    unsigned long   CapacityFrames;     // power of two

    // ---- Producer (app) ----
    volatile long long WriteFrame;      // total frames ever written (monotonic)
    volatile long      TargetFillFrames;// how far behind WriteFrame the driver re-syncs to

    // ---- Consumer (driver) ----
    volatile long      CaptureActive;   // 1 while a capture stream on the virtual mic is running
    volatile long long ReadFrame;       // total frames ever consumed by the driver
    volatile long      UnderrunCount;   // driver ran dry (app too slow / target too small)
    volatile long      ResyncCount;     // driver jumped to catch up (overrun / app restarted)

    unsigned char      Reserved[VOCALBRIDGE_RING_HEADER_SIZE - 56];
    // Samples follow at offset HeaderSize: long[CapacityFrames * Channels]
} VOCALBRIDGE_RING_HEADER;

typedef struct _VOCALBRIDGE_DRIVER_INFO
{
    unsigned long   ProtocolVersion;
    unsigned long   SampleRate;
    unsigned long   Channels;
    unsigned long   RingAttached;       // 1 if some process currently owns the ring
    unsigned long   CaptureActive;      // 1 if an app is capturing from the virtual mic
    unsigned long   Reserved[11];
} VOCALBRIDGE_DRIVER_INFO;

#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(VOCALBRIDGE_RING_HEADER) == VOCALBRIDGE_RING_HEADER_SIZE, "ring header size mismatch");
#endif

#endif // VOCALBRIDGE_SHARED_H
