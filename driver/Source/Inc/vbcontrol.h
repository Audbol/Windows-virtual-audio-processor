/*++

Module Name:

    vbcontrol.h

Abstract:

    VocalBridge control device + shared ring buffer.

    A small non-PnP "control device object" (\\.\VocalBridge) lets the VocalBridge
    host app hand the driver a ring buffer that lives in the app's memory. The
    virtual microphone's capture stream copies audio out of that ring directly
    into its WaveRT DMA buffer, so the processed voice reaches games with only the
    app's own buffer + ~1 ms of ring headroom of extra latency.

--*/

#ifndef _VOCALBRIDGE_VBCONTROL_H_
#define _VOCALBRIDGE_VBCONTROL_H_

// Hook every IRP major function so IRPs aimed at the control device are handled
// here and everything else still goes to PortCls. Call at the very end of
// DriverEntry, after PcInitializeAdapterDriver and any other MajorFunction edits.
VOID VbHookDispatchTable(_In_ PDRIVER_OBJECT DriverObject);

// Create / delete the control device object (PASSIVE_LEVEL).
NTSTATUS VbControlDeviceCreate(_In_ PDRIVER_OBJECT DriverObject);
VOID     VbControlDeviceDelete();

// Called by the capture stream when it enters / leaves KSSTATE_RUN.
VOID VbRingCaptureStart();
VOID VbRingCaptureStop();

// Fill 'ByteCount' bytes of capture DMA memory with ring audio (or silence).
// Destination format: 48 kHz / 2 ch / 32-bit PCM. Callable at <= DISPATCH_LEVEL.
VOID VbRingRead(_Out_writes_bytes_(ByteCount) BYTE* Destination, _In_ ULONG ByteCount);

#endif // _VOCALBRIDGE_VBCONTROL_H_
