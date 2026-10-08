/*++

Module Name:

    vbcontrol.cpp

Abstract:

    VocalBridge control device object and the shared ring buffer that feeds the
    virtual microphone.

    Synchronisation
    ---------------
    g_RingLock (spin lock) protects every g_Ring* global. The capture stream reads
    the ring with this lock held (it is already at DISPATCH_LEVEL under the
    stream's position lock), the IOCTL / cleanup / cancel paths attach and detach
    under it. Because the reader holds the lock for the whole copy, once a detach
    has run under the lock nobody touches the app's pages any more and the attach
    IRP may be completed (which unlocks those pages).

    Everything read from the shared header after attach is treated as untrusted:
    indices are always masked with the capacity we validated and cached at attach
    time, so a misbehaving app can only produce bad audio, never bad memory access.

--*/

#pragma warning (disable : 4127)

#include "definitions.h"
#include <wdmsec.h>
#include "vbcontrol.h"
#include "../../../shared/VocalBridgeShared.h"

#define VB_CONTROL_SIGNATURE    'DCBV'
#define VB_POOLTAG              'GBRV'

// Never let the virtual mic lag the app by more than this much (frames).
#define VB_MAX_EXTRA_LAG_FRAMES (VOCALBRIDGE_SAMPLE_RATE / 20)   // 50 ms
#define VB_MIN_TARGET_FRAMES    16

typedef struct _VB_CONTROL_EXTENSION
{
    ULONG   Signature;
} VB_CONTROL_EXTENSION, *PVB_CONTROL_EXTENSION;

static PDEVICE_OBJECT       g_ControlDevice = NULL;
static PDRIVER_DISPATCH     g_PortClsDispatch[IRP_MJ_MAXIMUM_FUNCTION + 1] = {};

static KSPIN_LOCK           g_RingLock;
static PIRP                 g_RingIrp = NULL;           // pending ATTACH IRP that owns the ring pages
static PFILE_OBJECT         g_RingFileObject = NULL;
static VOCALBRIDGE_RING_HEADER* g_RingHeader = NULL;    // system-space mapping of the app's buffer
static LONG*                g_RingSamples = NULL;
static ULONG                g_RingCapacityFrames = 0;   // validated copy, power of two
static LONGLONG             g_RingReadFrame = 0;        // driver-private read cursor
static BOOLEAN              g_RingNeedResync = TRUE;
static BOOLEAN              g_RingStarved = FALSE;
static BOOLEAN              g_CaptureActive = FALSE;

DRIVER_DISPATCH VbDispatchHook;
DRIVER_CANCEL   VbCancelAttach;

//=============================================================================
// Helpers
//=============================================================================
#pragma code_seg()
static BOOLEAN VbIsControlDevice(_In_ PDEVICE_OBJECT DeviceObject)
{
    // Identify by type + extension signature rather than by g_ControlDevice so
    // CLEANUP/CLOSE IRPs that arrive after VbControlDeviceDelete() (handle still
    // open while the device is being removed) are still routed to us.
    if (DeviceObject->DeviceType != VOCALBRIDGE_IOCTL_DEVICE_TYPE)
    {
        return FALSE;
    }
    PVB_CONTROL_EXTENSION ext = (PVB_CONTROL_EXTENSION)DeviceObject->DeviceExtension;
    return (ext != NULL && ext->Signature == VB_CONTROL_SIGNATURE);
}

#pragma code_seg()
static VOID VbCompleteIrp(_In_ PIRP Irp, _In_ NTSTATUS Status, _In_ ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

// Caller holds g_RingLock.
#pragma code_seg()
static VOID VbDetachLocked()
{
    if (g_RingHeader != NULL)
    {
        InterlockedExchange(&g_RingHeader->CaptureActive, 0);
    }
    g_RingIrp = NULL;
    g_RingFileObject = NULL;
    g_RingHeader = NULL;
    g_RingSamples = NULL;
    g_RingCapacityFrames = 0;
    g_RingReadFrame = 0;
    g_RingNeedResync = TRUE;
    g_RingStarved = FALSE;
}

//=============================================================================
// Ring consumer (capture stream)
//=============================================================================
#pragma code_seg()
VOID VbRingCaptureStart()
{
    KIRQL irql;
    KeAcquireSpinLock(&g_RingLock, &irql);
    g_CaptureActive = TRUE;
    g_RingNeedResync = TRUE;     // start from the freshest audio, not stale data
    if (g_RingHeader != NULL)
    {
        InterlockedExchange(&g_RingHeader->CaptureActive, 1);
    }
    KeReleaseSpinLock(&g_RingLock, irql);
}

#pragma code_seg()
VOID VbRingCaptureStop()
{
    KIRQL irql;
    KeAcquireSpinLock(&g_RingLock, &irql);
    g_CaptureActive = FALSE;
    if (g_RingHeader != NULL)
    {
        InterlockedExchange(&g_RingHeader->CaptureActive, 0);
    }
    KeReleaseSpinLock(&g_RingLock, irql);
}

#pragma code_seg()
VOID VbRingRead(_Out_writes_bytes_(ByteCount) BYTE* Destination, _In_ ULONG ByteCount)
{
    const ULONG framesWanted = ByteCount / VOCALBRIDGE_BYTES_PER_FRAME;
    ULONG       framesCopied = 0;
    KIRQL       irql;

    KeAcquireSpinLock(&g_RingLock, &irql);

    if (g_RingHeader != NULL && framesWanted > 0)
    {
        const ULONG    capacity = g_RingCapacityFrames;

        // Aligned 64-bit loads are single-copy atomic on x64 and ARM64; the barrier
        // gives acquire ordering (samples are read after the cursor that publishes them).
        const LONGLONG writeFrame = *(volatile LONGLONG*)&g_RingHeader->WriteFrame;
        KeMemoryBarrier();

        LONG target = *(volatile LONG*)&g_RingHeader->TargetFillFrames;
        if (target < VB_MIN_TARGET_FRAMES)       target = VB_MIN_TARGET_FRAMES;
        if (target > (LONG)(capacity / 2))      target = (LONG)(capacity / 2);

        LONGLONG fill = writeFrame - g_RingReadFrame;

        // (Re)synchronise: first read after attach/start, app restarted (cursor went
        // backwards), data already overwritten, or we drifted too far behind.
        if (g_RingNeedResync
            || fill < 0
            || fill > (LONGLONG)capacity - (LONGLONG)framesWanted
            || fill > (LONGLONG)target + VB_MAX_EXTRA_LAG_FRAMES + (LONGLONG)framesWanted)
        {
            if (!g_RingNeedResync)
            {
                InterlockedIncrement(&g_RingHeader->ResyncCount);
            }
            g_RingReadFrame = writeFrame - target;
            g_RingNeedResync = FALSE;
            g_RingStarved = FALSE;
            fill = target;
        }

        // After an underrun wait until the app has refilled to the target before
        // resuming, so we get one clean gap instead of a long crackle.
        if (g_RingStarved && fill >= target)
        {
            g_RingStarved = FALSE;
        }

        if (!g_RingStarved && fill > 0)
        {
            ULONG toCopy = framesWanted;
            if (fill < (LONGLONG)framesWanted)
            {
                toCopy = (ULONG)fill;
                g_RingStarved = TRUE;
                InterlockedIncrement(&g_RingHeader->UnderrunCount);
            }

            while (framesCopied < toCopy)
            {
                const ULONG index = (ULONG)(g_RingReadFrame & (LONGLONG)(capacity - 1));
                ULONG run = toCopy - framesCopied;
                if (run > capacity - index)
                {
                    run = capacity - index;
                }

                RtlCopyMemory(Destination + (SIZE_T)framesCopied * VOCALBRIDGE_BYTES_PER_FRAME,
                              g_RingSamples + (SIZE_T)index * VOCALBRIDGE_CHANNELS,
                              (SIZE_T)run * VOCALBRIDGE_BYTES_PER_FRAME);

                framesCopied += run;
                g_RingReadFrame += run;
            }
        }
        else if (!g_RingStarved)
        {
            // fill == 0: the app has not produced anything new yet.
            g_RingStarved = TRUE;
            InterlockedIncrement(&g_RingHeader->UnderrunCount);
        }

        InterlockedExchange64(&g_RingHeader->ReadFrame, g_RingReadFrame);
    }

    KeReleaseSpinLock(&g_RingLock, irql);

    const ULONG bytesCopied = framesCopied * VOCALBRIDGE_BYTES_PER_FRAME;
    if (bytesCopied < ByteCount)
    {
        RtlZeroMemory(Destination + bytesCopied, ByteCount - bytesCopied);
    }
}

//=============================================================================
// Attach / detach
//=============================================================================
#pragma code_seg()
VOID VbCancelAttach(_Inout_ PDEVICE_OBJECT DeviceObject, _Inout_ _IRQL_uses_cancel_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    IoReleaseCancelSpinLock(Irp->CancelIrql);

    KIRQL irql;
    KeAcquireSpinLock(&g_RingLock, &irql);
    if (g_RingIrp == Irp)
    {
        VbDetachLocked();
    }
    KeReleaseSpinLock(&g_RingLock, irql);

    VbCompleteIrp(Irp, STATUS_CANCELLED, 0);
}

// Detach the ring if it is owned by FileObject (or unconditionally if NULL) and
// complete the attach IRP with Status.
#pragma code_seg()
static VOID VbDetach(_In_opt_ PFILE_OBJECT FileObject, _In_ NTSTATUS Status)
{
    PIRP  irpToComplete = NULL;
    KIRQL irql;

    KeAcquireSpinLock(&g_RingLock, &irql);
    if (g_RingIrp != NULL && (FileObject == NULL || g_RingFileObject == FileObject))
    {
        PIRP irp = g_RingIrp;
        if (IoSetCancelRoutine(irp, NULL) != NULL)
        {
            // We own the IRP now.
            VbDetachLocked();
            irpToComplete = irp;
        }
        // else: the cancel routine is already running; it will detach + complete.
    }
    KeReleaseSpinLock(&g_RingLock, irql);

    if (irpToComplete != NULL)
    {
        VbCompleteIrp(irpToComplete, Status, 0);
    }
}

#pragma code_seg()
static NTSTATUS VbAttach(_In_ PIRP Irp, _In_ PIO_STACK_LOCATION Stack)
{
    const ULONG length = Stack->Parameters.DeviceIoControl.OutputBufferLength;

    if (Irp->MdlAddress == NULL || length < VOCALBRIDGE_RING_HEADER_SIZE)
    {
        VbCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
        return STATUS_INVALID_PARAMETER;
    }

    BYTE* base = (BYTE*)MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute);
    if (base == NULL)
    {
        VbCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (((ULONG_PTR)base & 7) != 0)
    {
        VbCompleteIrp(Irp, STATUS_DATATYPE_MISALIGNMENT, 0);
        return STATUS_DATATYPE_MISALIGNMENT;
    }

    // Snapshot the app-controlled fields once, validate the snapshot, and only use
    // the snapshot afterwards.
    VOCALBRIDGE_RING_HEADER* header = (VOCALBRIDGE_RING_HEADER*)base;
    const ULONG magic      = *(volatile ULONG*)&header->Magic;
    const ULONG version    = *(volatile ULONG*)&header->Version;
    const ULONG headerSize = *(volatile ULONG*)&header->HeaderSize;
    const ULONG sampleRate = *(volatile ULONG*)&header->SampleRate;
    const ULONG channels   = *(volatile ULONG*)&header->Channels;
    const ULONG capacity   = *(volatile ULONG*)&header->CapacityFrames;

    const BOOLEAN valid =
           magic == VOCALBRIDGE_RING_MAGIC
        && version == VOCALBRIDGE_PROTOCOL_VERSION
        && headerSize == VOCALBRIDGE_RING_HEADER_SIZE
        && sampleRate == VOCALBRIDGE_SAMPLE_RATE
        && channels == VOCALBRIDGE_CHANNELS
        && capacity >= VOCALBRIDGE_MIN_CAPACITY_FRAMES
        && capacity <= VOCALBRIDGE_MAX_CAPACITY_FRAMES
        && (capacity & (capacity - 1)) == 0
        && (ULONGLONG)VOCALBRIDGE_RING_HEADER_SIZE + (ULONGLONG)capacity * VOCALBRIDGE_BYTES_PER_FRAME <= (ULONGLONG)length;

    if (!valid)
    {
        VbCompleteIrp(Irp, STATUS_REVISION_MISMATCH, 0);
        return STATUS_REVISION_MISMATCH;
    }

    KIRQL irql;
    KeAcquireSpinLock(&g_RingLock, &irql);

    if (g_RingIrp != NULL)
    {
        KeReleaseSpinLock(&g_RingLock, irql);
        VbCompleteIrp(Irp, STATUS_DEVICE_BUSY, 0);
        return STATUS_DEVICE_BUSY;
    }

    IoMarkIrpPending(Irp);
    IoSetCancelRoutine(Irp, VbCancelAttach);

    if (Irp->Cancel)
    {
        if (IoSetCancelRoutine(Irp, NULL) != NULL)
        {
            KeReleaseSpinLock(&g_RingLock, irql);
            VbCompleteIrp(Irp, STATUS_CANCELLED, 0);
            return STATUS_PENDING;
        }
        // Cancel routine is running and will complete the IRP (it won't find it
        // attached, so it won't detach anything).
        KeReleaseSpinLock(&g_RingLock, irql);
        return STATUS_PENDING;
    }

    g_RingIrp            = Irp;
    g_RingFileObject     = Stack->FileObject;
    g_RingHeader         = header;
    g_RingSamples        = (LONG*)(base + VOCALBRIDGE_RING_HEADER_SIZE);
    g_RingCapacityFrames = capacity;
    g_RingReadFrame      = 0;
    g_RingNeedResync     = TRUE;
    g_RingStarved        = FALSE;
    InterlockedExchange(&header->CaptureActive, g_CaptureActive ? 1 : 0);

    KeReleaseSpinLock(&g_RingLock, irql);

    DPF(D_TERSE, ("[VocalBridge] ring attached, %u frames", capacity));
    return STATUS_PENDING;
}

#pragma code_seg()
static NTSTATUS VbGetInfo(_In_ PIRP Irp, _In_ PIO_STACK_LOCATION Stack)
{
    if (Stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(VOCALBRIDGE_DRIVER_INFO))
    {
        VbCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        return STATUS_BUFFER_TOO_SMALL;
    }

    VOCALBRIDGE_DRIVER_INFO* info = (VOCALBRIDGE_DRIVER_INFO*)Irp->AssociatedIrp.SystemBuffer;
    RtlZeroMemory(info, sizeof(*info));
    info->ProtocolVersion = VOCALBRIDGE_PROTOCOL_VERSION;
    info->SampleRate      = VOCALBRIDGE_SAMPLE_RATE;
    info->Channels        = VOCALBRIDGE_CHANNELS;

    KIRQL irql;
    KeAcquireSpinLock(&g_RingLock, &irql);
    info->RingAttached  = (g_RingIrp != NULL) ? 1 : 0;
    info->CaptureActive = g_CaptureActive ? 1 : 0;
    KeReleaseSpinLock(&g_RingLock, irql);

    VbCompleteIrp(Irp, STATUS_SUCCESS, sizeof(*info));
    return STATUS_SUCCESS;
}

//=============================================================================
// Dispatch
//=============================================================================
#pragma code_seg()
static NTSTATUS VbControlDispatch(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    switch (stack->MajorFunction)
    {
    case IRP_MJ_CREATE:
    case IRP_MJ_CLOSE:
        VbCompleteIrp(Irp, STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;

    case IRP_MJ_CLEANUP:
        // Handle is being closed: release the ring if this handle owns it.
        VbDetach(stack->FileObject, STATUS_CANCELLED);
        VbCompleteIrp(Irp, STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;

    case IRP_MJ_DEVICE_CONTROL:
        switch (stack->Parameters.DeviceIoControl.IoControlCode)
        {
        case IOCTL_VOCALBRIDGE_ATTACH_RING:
            return VbAttach(Irp, stack);
        case IOCTL_VOCALBRIDGE_GET_INFO:
            return VbGetInfo(Irp, stack);
        default:
            break;
        }
        break;

    default:
        break;
    }

    VbCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    return STATUS_INVALID_DEVICE_REQUEST;
}

#pragma code_seg()
NTSTATUS VbDispatchHook(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    if (VbIsControlDevice(DeviceObject))
    {
        return VbControlDispatch(DeviceObject, Irp);
    }

    const UCHAR major = IoGetCurrentIrpStackLocation(Irp)->MajorFunction;
    PDRIVER_DISPATCH original = (major <= IRP_MJ_MAXIMUM_FUNCTION) ? g_PortClsDispatch[major] : NULL;
    if (original == NULL)
    {
        VbCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    return original(DeviceObject, Irp);
}

#pragma code_seg("INIT")
VOID VbHookDispatchTable(_In_ PDRIVER_OBJECT DriverObject)
{
    KeInitializeSpinLock(&g_RingLock);

    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i)
    {
        g_PortClsDispatch[i] = DriverObject->MajorFunction[i];
        DriverObject->MajorFunction[i] = VbDispatchHook;
    }
}

//=============================================================================
// Control device lifetime
//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS VbControlDeviceCreate(_In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();

    if (g_ControlDevice != NULL)
    {
        return STATUS_SUCCESS;
    }

    static const GUID classGuid = VOCALBRIDGE_CONTROL_CLASS_GUID_INIT;
    UNICODE_STRING deviceName;
    UNICODE_STRING linkName;
    PDEVICE_OBJECT device = NULL;

    RtlInitUnicodeString(&deviceName, VOCALBRIDGE_KERNEL_DEVICE_NAME);
    RtlInitUnicodeString(&linkName, VOCALBRIDGE_KERNEL_SYMLINK_NAME);

    // Any local user may feed the virtual mic (that is the whole point of the app);
    // only SYSTEM/Administrators get full control of the object.
    NTSTATUS status = IoCreateDeviceSecure(DriverObject,
                                           sizeof(VB_CONTROL_EXTENSION),
                                           &deviceName,
                                           VOCALBRIDGE_IOCTL_DEVICE_TYPE,
                                           FILE_DEVICE_SECURE_OPEN,
                                           FALSE,
                                           &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R,
                                           &classGuid,
                                           &device);
    if (!NT_SUCCESS(status))
    {
        DPF(D_ERROR, ("[VocalBridge] IoCreateDeviceSecure failed 0x%x", status));
        return status;
    }

    ((PVB_CONTROL_EXTENSION)device->DeviceExtension)->Signature = VB_CONTROL_SIGNATURE;

    status = IoCreateSymbolicLink(&linkName, &deviceName);
    if (!NT_SUCCESS(status))
    {
        DPF(D_ERROR, ("[VocalBridge] IoCreateSymbolicLink failed 0x%x", status));
        IoDeleteDevice(device);
        return status;
    }

    device->Flags &= ~DO_DEVICE_INITIALIZING;
    g_ControlDevice = device;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
VOID VbControlDeviceDelete()
{
    PAGED_CODE();

    VbDetach(NULL, STATUS_DEVICE_REMOVED);

    if (g_ControlDevice != NULL)
    {
        UNICODE_STRING linkName;
        RtlInitUnicodeString(&linkName, VOCALBRIDGE_KERNEL_SYMLINK_NAME);
        IoDeleteSymbolicLink(&linkName);
        IoDeleteDevice(g_ControlDevice);
        g_ControlDevice = NULL;
    }
}
#pragma code_seg()
