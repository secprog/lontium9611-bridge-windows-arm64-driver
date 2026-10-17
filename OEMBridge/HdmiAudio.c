/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

\

#include "driver.h"
#include <wdmguid.h>
#include <ntstrsafe.h>

#include <windef.h>
#include <ks.h>
#include <ksmedia.h>
#include "HdmiAudioAbi.h"

C_ASSERT(sizeof(HDMI_AUDIO_PROVIDER_API) == HDMI_AUDIO_FXN_TABLE_SIZE);
C_ASSERT(sizeof(HDMI_AUDIO_CONSUMER_API) == HDMI_AUDIO_FXN_TABLE_SIZE);
C_ASSERT(FIELD_OFFSET(HDMI_AUDIO_PROVIDER_API, GetModes) == 0x30);
C_ASSERT(FIELD_OFFSET(HDMI_AUDIO_PROVIDER_API, ChangeState) == 0x58);

#define OURS(h)   ((HDMI_AUDIO_PROVIDER_API *)(h)->OursTable)
#define AUDIO(h)  ((HDMI_AUDIO_CONSUMER_API *)(h)->AudioTable)

#define HDMI_AUDIO_PORT_INDEX       0
#define HDMI_AUDIO_BITDEPTH_32BIT   0x08
#define HDMI_AUDIO_RETRY_MS         2000
#define HDMI_AUDIO_MAX_RETRIES      90

static const GUID GUID_QC_AUDIO_MINIPORT_HDMI =
    { 0xfc2e6f58, 0x5abb, 0x4a26, { 0x8a, 0x85, 0x35, 0x23, 0xa2, 0xec, 0xba, 0x60 } };

static const GUID GUID_HDMI_AUDIO_CONTAINER =
    { 0x7b3c1e9a, 0x6d2f, 0x4c48, { 0x9a, 0x1e, 0x5e, 0x0d, 0x2c, 0x8f, 0x4b, 0x10 } };

static const GUID GUID_KSDATAFORMAT_SUBTYPE_LPCM_AUDIO =
    { 0xe06d8032, 0xdb46, 0x11cf, { 0xb4, 0xd1, 0x00, 0x80, 0x5f, 0x6c, 0xbb, 0xea } };

static const GUID GUID_KSCATEGORY_AUDIO_LOCAL =
    { 0x6994ad04, 0x93ef, 0x11d0, { 0xa3, 0xcc, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } };

static
PHDMI_AUDIO_CONTEXT
HdmiAudioFromDevice(
    _In_ WDFDEVICE Device
    )
{
    return &DeviceGetContext(Device)->HdmiAudio;
}

static BOOLEAN
HdmiAudioReadEnabled(_In_ WDFDEVICE Device)
{
    DECLARE_CONST_UNICODE_STRING(name, L"HdmiAudio");
    WDFKEY key;
    ULONG enabled = 1;

    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ,
                                            WDF_NO_OBJECT_ATTRIBUTES, &key)))
    {
        if (!NT_SUCCESS(WdfRegistryQueryULong(key, &name, &enabled)))
        {
            enabled = 1;
        }
        WdfRegistryClose(key);
    }
    return enabled != 0;
}

static NTSTATUS
HdmiAudioWriteEnabled(_In_ WDFDEVICE Device, _In_ BOOLEAN Enabled)
{
    DECLARE_CONST_UNICODE_STRING(name, L"HdmiAudio");
    WDFKEY key;
    NTSTATUS status;

    status = WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE, KEY_SET_VALUE,
                                      WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    status = WdfRegistryAssignULong(key, &name, Enabled ? 1 : 0);
    WdfRegistryClose(key);
    return status;
}

VOID
HdmiAudioLoadSettings(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT dc = DeviceGetContext(Device);
    PHDMI_AUDIO_CONTEXT h = HdmiAudioFromDevice(Device);
    BOOLEAN enabled = HdmiAudioReadEnabled(Device);

    WdfWaitLockAcquire(dc->BridgeLock, NULL);
    if (h->Lock != NULL) WdfWaitLockAcquire(h->Lock, NULL);
    h->Enabled = enabled;
    if (h->Lock != NULL) WdfWaitLockRelease(h->Lock);
    WdfWaitLockRelease(dc->BridgeLock);
}

BOOLEAN
HdmiAudioIsEnabled(_In_ WDFDEVICE Device)
{
    return HdmiAudioFromDevice(Device)->Enabled;
}

static NTSTATUS
HdmiAudioSetEnabled(_In_ WDFDEVICE Device, _In_ BOOLEAN Enabled)
{
    PDEVICE_CONTEXT dc = DeviceGetContext(Device);
    PHDMI_AUDIO_CONTEXT h = HdmiAudioFromDevice(Device);
    NTSTATUS status;

    WdfWaitLockAcquire(dc->BridgeLock, NULL);
    status = HdmiAudioWriteEnabled(Device, Enabled);
    if (!NT_SUCCESS(status))
    {
        WdfWaitLockRelease(dc->BridgeLock);
        return status;
    }
    if (h->Lock != NULL) WdfWaitLockAcquire(h->Lock, NULL);
    h->Enabled = Enabled;
    if (Enabled) h->Retries = 0;
    if (h->Lock != NULL) WdfWaitLockRelease(h->Lock);

    status = dc->BridgeReady ? LT9611AudioEnable(Device) : STATUS_SUCCESS;
    WdfWaitLockRelease(dc->BridgeLock);

    if (h->Available) WdfWorkItemEnqueue(h->WorkItem);
    return status;
}

BOOLEAN
HdmiAudioHandleIoctl(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ ULONG IoControlCode)
{
    NTSTATUS status;
    size_t length = 0;

    if (IoControlCode == IOCTL_OEMBRIDGE_AUDIO_GET_STATE)
    {
        PDEVICE_CONTEXT dc = DeviceGetContext(Device);
        OEMBRIDGE_AUDIO_STATE *state = NULL;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*state), &state, &length);
        if (!NT_SUCCESS(status))
        {
            WdfRequestComplete(Request, status);
            return TRUE;
        }
        RtlZeroMemory(state, sizeof(*state));
        state->Size = sizeof(*state);
        WdfWaitLockAcquire(dc->BridgeLock, NULL);
        state->Enabled = HdmiAudioIsEnabled(Device) ? 1 : 0;
        WdfWaitLockRelease(dc->BridgeLock);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*state));
        return TRUE;
    }
    if (IoControlCode == IOCTL_OEMBRIDGE_AUDIO_SET_ENABLE)
    {
        ULONG *enabled = NULL;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*enabled), &enabled, &length);
        if (!NT_SUCCESS(status))
        {
            WdfRequestComplete(Request, status);
            return TRUE;
        }
        if (*enabled > 1)
        {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return TRUE;
        }
        status = HdmiAudioSetEnabled(Device, *enabled != 0);
        WdfRequestComplete(Request, status);
        return TRUE;
    }
    return FALSE;
}

static
NTSTATUS
HdmiGfx_GetAudioModeInfo(
    PVOID Context,
    HDMI_AUDIO_MODES *pAudioModeInfo
    )
{
    PHDMI_AUDIO_CONTEXT h = (PHDMI_AUDIO_CONTEXT)Context;
    HDMI_AUDIO_FORMAT *f;

    if (h == NULL || pAudioModeInfo == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(pAudioModeInfo, sizeof(*pAudioModeInfo));
    pAudioModeInfo->Count = 1;
    f = &pAudioModeInfo->Formats[0];
    f->Encoding = GUID_KSDATAFORMAT_SUBTYPE_LPCM_AUDIO;
    f->Channels = 2;
    f->SampleRates = HDMI_AUDIO_RATE_48000;
    f->DepthValid = TRUE;

    f->Depths = HDMI_AUDIO_DEPTH_16 | HDMI_AUDIO_DEPTH_24 | HDMI_AUDIO_BITDEPTH_32BIT;
    f->BitRateValid = FALSE;
    f->BitRate = 0;
    f->Speakers = HDMI_AUDIO_STEREO;

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "hdmiaudio: GetAudioModeInfo -> 1 format (LPCM 2ch 48k 16/24/32, FL_FR)");
    return STATUS_SUCCESS;
}

static
NTSTATUS
HdmiGfx_SetAudioProperties(
    PVOID Context,
    HDMI_AUDIO_PROPERTIES *pAudioProperties
    )
{
    PHDMI_AUDIO_CONTEXT h = (PHDMI_AUDIO_CONTEXT)Context;

    if (Context == NULL || pAudioProperties == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
          "hdmiaudio: SetAudioProperties ch=%u rate=0x%x depth=0x%x downmix-inhibit=%d shift=%u spk=0x%x",
          pAudioProperties->Channels, pAudioProperties->SampleRate, pAudioProperties->Depth,
          pAudioProperties->InhibitDownmix, pAudioProperties->LevelShift, pAudioProperties->Speakers);

    if (pAudioProperties->SampleRate != HDMI_AUDIO_RATE_48000)
    {
        Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER, "hdmiaudio: refusing non-48k rate 0x%x", pAudioProperties->SampleRate);
        return STATUS_NOT_SUPPORTED;
    }

    h->FormatRateHz = 48000;
    switch ((ULONG)pAudioProperties->Depth)
    {
    case HDMI_AUDIO_DEPTH_16:   h->FormatBits = 16; break;
    case HDMI_AUDIO_DEPTH_20:   h->FormatBits = 20; break;
    case HDMI_AUDIO_DEPTH_24:   h->FormatBits = 24; break;
    case HDMI_AUDIO_BITDEPTH_32BIT:     h->FormatBits = 32; break;
    default:                            h->FormatBits = 16; break;
    }
    h->FormatChannels = pAudioProperties->Channels;
    WdfWorkItemEnqueue(h->FormatWorkItem);
    return STATUS_SUCCESS;
}

static
VOID
HdmiAudioEdidMonitorName(
    _In_ const UCHAR *Edid,
    _Out_writes_(Count) WCHAR *Name,
    _In_ ULONG Count
    )
{
    ULONG d, i, n = 0;

    Name[0] = 0;

    for (d = 0; d < 4; d++)
    {
        const UCHAR *desc = Edid + 0x36 + 18 * d;
        if (desc[0] == 0 && desc[1] == 0 && desc[2] == 0 && desc[3] == 0xFC)
        {
            for (i = 0; i < 13 && n + 1 < Count; i++)
            {
                UCHAR c = desc[5 + i];
                if (c == 0x0A || c == 0) break;
                Name[n++] = (c >= 0x20 && c < 0x7F) ? (WCHAR)c : L'?';
            }
            while (n > 0 && Name[n - 1] == L' ') n--;
            Name[n] = 0;
            return;
        }
    }
}

static
NTSTATUS
HdmiGfx_GetJackSinkInfo(
    PVOID Context,
    HDMI_AUDIO_SINK *pAudioJackInfo
    )
{
    PHDMI_AUDIO_CONTEXT h = (PHDMI_AUDIO_CONTEXT)Context;
    UCHAR edid[HDMI_AUDIO_EDID_BLOCK];
    BOOLEAN valid;
    WCHAR name[32];

    if (h == NULL || pAudioJackInfo == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    WdfWaitLockAcquire(h->Lock, NULL);
    valid = h->EdidValid;
    RtlCopyMemory(edid, h->Edid, sizeof(edid));
    WdfWaitLockRelease(h->Lock);

    RtlZeroMemory(pAudioJackInfo, sizeof(*pAudioJackInfo));
    pAudioJackInfo->Jack.ConnType = KSJACK_SINK_CONNECTIONTYPE_HDMI;
    pAudioJackInfo->Jack.HDCPCapable = FALSE;
    pAudioJackInfo->Jack.AICapable = FALSE;
    pAudioJackInfo->Jack.AudioLatency = 0;
    pAudioJackInfo->Jack.PortId.LowPart = 0;
    pAudioJackInfo->Jack.PortId.HighPart = 0;
    pAudioJackInfo->Container = GUID_HDMI_AUDIO_CONTAINER;

    name[0] = 0;
    if (valid)
    {
        pAudioJackInfo->Jack.ManufacturerId = ((USHORT)edid[8] << 8) | edid[9];
        pAudioJackInfo->Jack.ProductId = (USHORT)(edid[10] | ((USHORT)edid[11] << 8));
        HdmiAudioEdidMonitorName(edid, name, ARRAYSIZE(name));
    }
    if (name[0] == 0)
    {
        RtlStringCchCopyW(name, ARRAYSIZE(name), L"HDMI");
    }
    RtlStringCchCopyW(pAudioJackInfo->Jack.SinkDescription,
                      ARRAYSIZE(pAudioJackInfo->Jack.SinkDescription), name);
    pAudioJackInfo->Jack.SinkDescriptionLength = (UCHAR)wcslen(pAudioJackInfo->Jack.SinkDescription);

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "hdmiaudio: GetJackSinkInfo -> '%ws' mfg 0x%04x prod 0x%04x (edid %s)",
          pAudioJackInfo->Jack.SinkDescription, pAudioJackInfo->Jack.ManufacturerId,
          pAudioJackInfo->Jack.ProductId, valid ? "cached" : "none");
    return STATUS_SUCCESS;
}

static
NTSTATUS
HdmiGfx_SetAudioHDCP(
    PVOID Context,
    BOOL enable
    )
{
    UNREFERENCED_PARAMETER(Context);
    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "hdmiaudio: SetAudioHDCP(%d) -> ignored", enable);
    return STATUS_SUCCESS;
}

static
NTSTATUS
HdmiGfx_GetRoutingInfo(
    PVOID Context,
    HDMI_AUDIO_ROUTING *pRoutingInfo
    )
{
    UNREFERENCED_PARAMETER(Context);
    if (pRoutingInfo == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(pRoutingInfo, sizeof(*pRoutingInfo));
    pRoutingInfo->Controller = 0;
    pRoutingInfo->Stream = 0;
    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "hdmiaudio: GetRoutingInfo -> controller 0, stream 0");
    return STATUS_SUCCESS;
}

static
NTSTATUS
HdmiGfx_ChangeState(
    PVOID Context,
    ULONG eStateType
    )
{
    PHDMI_AUDIO_CONTEXT h = (PHDMI_AUDIO_CONTEXT)Context;
    LONG n;

    if (h == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (eStateType == HDMI_AUDIO_STREAM_START)
    {
        n = InterlockedIncrement(&h->StreamsRunning);
    }
    else
    {
        n = InterlockedDecrement(&h->StreamsRunning);
        if (n < 0) { InterlockedExchange(&h->StreamsRunning, 0); n = 0; }
    }
    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "hdmiaudio: ChangeState %s (streams=%ld)",
          eStateType == HDMI_AUDIO_STREAM_START ? "START" : "STOP", n);
    return STATUS_SUCCESS;
}

static NTSTATUS HdmiAudioFindKsRoute(_In_ PHDMI_AUDIO_CONTEXT H);

EVT_WDF_IO_TARGET_QUERY_REMOVE     HdmiAudioEvtTargetQueryRemove;
EVT_WDF_IO_TARGET_REMOVE_CANCELED  HdmiAudioEvtTargetRemoveCanceled;
EVT_WDF_IO_TARGET_REMOVE_COMPLETE  HdmiAudioEvtTargetRemoveComplete;

static
VOID
HdmiAudioDropTarget(
    _In_ PHDMI_AUDIO_CONTEXT H
    )
{
    if (H->Target != NULL)
    {
        WdfIoTargetClose(H->Target);
        WdfObjectDelete(H->Target);
        H->Target = NULL;
    }
    RtlZeroMemory(H->AudioTable, sizeof(H->AudioTable));
    H->Established = FALSE;
    H->ReportedPresent = FALSE;
}

static
NTSTATUS
HdmiAudioEstablish(
    _In_ PHDMI_AUDIO_CONTEXT H
    )

{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_TARGET_OPEN_PARAMS openParams;
    WDF_MEMORY_DESCRIPTOR inDesc, outDesc;
    ULONG_PTR bytes = 0;

    if (H->Established)
    {
        return STATUS_SUCCESS;
    }
    if (H->SymbolicLink.Length == 0)
    {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (H->Target != NULL)
    {

        WdfObjectDelete(H->Target);
        H->Target = NULL;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = H->FxDevice;
    status = WdfIoTargetCreate(H->FxDevice, &attributes, &H->Target);
    if (!NT_SUCCESS(status))
    {
        H->Target = NULL;
        return status;
    }

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&openParams, &H->SymbolicLink, GENERIC_READ | GENERIC_WRITE);
    openParams.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;

    openParams.EvtIoTargetQueryRemove    = HdmiAudioEvtTargetQueryRemove;
    openParams.EvtIoTargetRemoveCanceled = HdmiAudioEvtTargetRemoveCanceled;
    openParams.EvtIoTargetRemoveComplete = HdmiAudioEvtTargetRemoveComplete;
    status = WdfIoTargetOpen(H->Target, &openParams);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER, "hdmiaudio: cannot open miniport HDMI interface %wZ (0x%08lX)",
              &H->SymbolicLink, status);
        WdfObjectDelete(H->Target);
        H->Target = NULL;
        return status;
    }

    RtlZeroMemory(H->OursTable, sizeof(H->OursTable));
    OURS(H)->Major = HDMI_AUDIO_API_MAJOR;
    OURS(H)->Minor = HDMI_AUDIO_API_MINOR;
    OURS(H)->Context = H;

    OURS(H)->PortIndex = HDMI_AUDIO_PORT_INDEX;
    OURS(H)->GetModes   = HdmiGfx_GetAudioModeInfo;
    OURS(H)->SetProperties = HdmiGfx_SetAudioProperties;
    OURS(H)->GetSink    = HdmiGfx_GetJackSinkInfo;
    OURS(H)->SetHdcp       = HdmiGfx_SetAudioHDCP;
    OURS(H)->GetRouting     = HdmiGfx_GetRoutingInfo;
    OURS(H)->ChangeState        = HdmiGfx_ChangeState;
    RtlZeroMemory(H->AudioTable, sizeof(H->AudioTable));

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inDesc, H->OursTable, sizeof(H->OursTable));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outDesc, H->AudioTable, sizeof(H->AudioTable));
    status = WdfIoTargetSendIoctlSynchronously(H->Target, NULL, HDMI_AUDIO_HOTPLUG_IOCTL,
                                               &inDesc, &outDesc, NULL, &bytes);
    if (!NT_SUCCESS(status) || bytes != sizeof(H->AudioTable) || AUDIO(H)->Hotplug == NULL)
    {
        if (status == STATUS_UNSUCCESSFUL && bytes == 0)
        {

            Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
                  "hdmiaudio: miniport HDMI manager not ready yet (0xC0000001) - retrying");
        }
        else
        {
            Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                  "hdmiaudio: SET_HDMI_HOT_PLUG_API_INTERFACE failed 0x%08lX (returned %Iu B, hotplug fn %p)",
                  status, bytes, AUDIO(H)->Hotplug);
        }
        if (NT_SUCCESS(status))
        {
            status = STATUS_INVALID_DEVICE_STATE;
        }
        WdfIoTargetClose(H->Target);
        WdfObjectDelete(H->Target);
        H->Target = NULL;
        RtlZeroMemory(H->AudioTable, sizeof(H->AudioTable));
        return status;
    }

    H->Established = TRUE;
    H->Retries = 0;
    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
          "hdmiaudio: linked to the audio miniport (%wZ): audio-side v%u.%u, Context %p, our port index %u",
          &H->SymbolicLink, AUDIO(H)->Major, AUDIO(H)->Minor, AUDIO(H)->Context,
          OURS(H)->PortIndex);
    return STATUS_SUCCESS;
}

VOID
HdmiAudioGetFormat(
    _In_ WDFDEVICE Device,
    _Out_ PULONG RateHz,
    _Out_ PULONG Bits
    )
{
    PHDMI_AUDIO_CONTEXT h = HdmiAudioFromDevice(Device);
    *RateHz = h->FormatRateHz ? h->FormatRateHz : 48000;
    *Bits = h->FormatBits ? h->FormatBits : 16;
}

static
VOID
HdmiAudioEvtFormatWorkItem(
    _In_ WDFWORKITEM WorkItem
    )
{
    WDFDEVICE device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
    PDEVICE_CONTEXT dc = DeviceGetContext(device);
    PHDMI_AUDIO_CONTEXT h = HdmiAudioFromDevice(device);
    NTSTATUS status;

    WdfWaitLockAcquire(dc->BridgeLock, NULL);
    if (dc->BridgeReady)
    {
        status = LT9611AudioEnable(device);
        Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
              "hdmiaudio: bridge re-armed for %lu Hz / %lu-bit / %lu ch -> 0x%08lX",
              h->FormatRateHz, h->FormatBits, h->FormatChannels, status);
    }
    else
    {
        Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER,
              "hdmiaudio: bridge not ready, format %lu Hz / %lu-bit kept for the next relock",
              h->FormatRateHz, h->FormatBits);
    }
    WdfWaitLockRelease(dc->BridgeLock);
}

static
VOID
HdmiAudioEvtWorkItem(
    _In_ WDFWORKITEM WorkItem
    )
{
    PHDMI_AUDIO_CONTEXT h = HdmiAudioFromDevice((WDFDEVICE)WdfWorkItemGetParentObject(WorkItem));
    NTSTATUS status;
    BOOLEAN retry = FALSE;
    BOOLEAN want, reported;
    NTSTATUS (*hotplug)(PVOID, BOOL) = NULL;
    PVOID ctx = NULL;

    WdfWaitLockAcquire(h->CallLock, NULL);
    WdfWaitLockAcquire(h->Lock, NULL);
    if (!h->Active)
    {
        WdfWaitLockRelease(h->Lock);
        WdfWaitLockRelease(h->CallLock);
        return;
    }
    if (!h->Established && h->Enabled)
    {
        if (h->SymbolicLink.Length == 0 && h->KsRouteAvailable)
        {
            (VOID)HdmiAudioFindKsRoute(h);
        }
        status = HdmiAudioEstablish(h);
        h->LastStatus = status;
        if (!NT_SUCCESS(status))
        {
            retry = (h->SymbolicLink.Length != 0);
        }
    }
    want = h->Enabled && h->WantPresent;
    reported = h->ReportedPresent;
    if (h->Established && want != reported)
    {
        hotplug = AUDIO(h)->Hotplug;
        ctx = AUDIO(h)->Context;
        h->ReportedPresent = want;
    }
    WdfWaitLockRelease(h->Lock);

    if (hotplug != NULL)
    {

        status = hotplug(ctx, want ? TRUE : FALSE);
        Trace(NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_WARNING, TRACE_DRIVER,
              "hdmiaudio: HotPlugNotification(%s) -> 0x%08lX", want ? "PLUG" : "UNPLUG", status);
        if (!NT_SUCCESS(status))
        {
            WdfWaitLockAcquire(h->Lock, NULL);
            h->ReportedPresent = reported;
            WdfWaitLockRelease(h->Lock);
            retry = TRUE;
        }
    }
    WdfWaitLockRelease(h->CallLock);

    if (retry)
    {
        if (h->Retries < HDMI_AUDIO_MAX_RETRIES)
        {
            h->Retries++;
            WdfTimerStart(h->RetryTimer, WDF_REL_TIMEOUT_IN_MS(HDMI_AUDIO_RETRY_MS));
        }
        else
        {
            Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                  "hdmiaudio: giving up after %lu attempts (last 0x%08lX) - no ExternalDisplay endpoint",
                  h->Retries, h->LastStatus);
        }
    }
}

static
VOID
HdmiAudioEvtRetryTimer(
    _In_ WDFTIMER Timer
    )
{
    PHDMI_AUDIO_CONTEXT h = HdmiAudioFromDevice((WDFDEVICE)WdfTimerGetParentObject(Timer));
    WdfWorkItemEnqueue(h->WorkItem);
}

static
BOOLEAN
HdmiAudioContainsNoCase(
    _In_ PCUNICODE_STRING S,
    _In_ PCWSTR Tag
    )
{
    USHORT n = (USHORT)(S->Length / sizeof(WCHAR));
    USHORT m = 0, i, j;

    while (Tag[m] != L'\0') m++;
    if (m == 0 || n < m) return FALSE;
    for (i = 0; i + m <= n; i++)
    {
        for (j = 0; j < m; j++)
        {
            if (RtlUpcaseUnicodeChar(S->Buffer[i + j]) != RtlUpcaseUnicodeChar(Tag[j])) break;
        }
        if (j == m) return TRUE;
    }
    return FALSE;
}

static
NTSTATUS
HdmiAudioDeriveKsRoute(
    _In_ PCUNICODE_STRING KsLink,
    _Out_ PUNICODE_STRING Route
    )

{
    USHORT n = (USHORT)(KsLink->Length / sizeof(WCHAR));
    USHORT end = 0, i;

    for (i = 0; i < n; i++)
    {
        if (KsLink->Buffer[i] == L'}') end = (USHORT)(i + 1);
    }
    if (end == 0)
    {
        return STATUS_OBJECT_NAME_INVALID;
    }
    if ((ULONG)end * sizeof(WCHAR) + sizeof(HDMI_AUDIO_REF_STRING_SLASH) > Route->MaximumLength)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyMemory(Route->Buffer, KsLink->Buffer, end * sizeof(WCHAR));
    Route->Length = (USHORT)(end * sizeof(WCHAR));
    return RtlAppendUnicodeToString(Route, HDMI_AUDIO_REF_STRING_SLASH);
}

static
NTSTATUS
HdmiAudioKsLinkNotification(
    _In_ PVOID NotificationStructure,
    _Inout_opt_ PVOID Context
    )

{
    PDEVICE_INTERFACE_CHANGE_NOTIFICATION n = (PDEVICE_INTERFACE_CHANGE_NOTIFICATION)NotificationStructure;
    WDFDEVICE device = (WDFDEVICE)Context;
    PHDMI_AUDIO_CONTEXT h;
    UNICODE_STRING route;
    WCHAR routeBuffer[320];
    NTSTATUS status;
    BOOLEAN kick = FALSE;

    if (device == NULL || n->SymbolicLinkName == NULL ||
        !IsEqualGUID(&n->Event, &GUID_DEVICE_INTERFACE_ARRIVAL) ||
        !HdmiAudioContainsNoCase(n->SymbolicLinkName, HDMI_AUDIO_MINIPORT_TAG))
    {
        return STATUS_SUCCESS;
    }

    if (HdmiAudioContainsNoCase(n->SymbolicLinkName, L"Mic"))
    {
        return STATUS_SUCCESS;
    }
    h = HdmiAudioFromDevice(device);

    RtlInitEmptyUnicodeString(&route, routeBuffer, sizeof(routeBuffer));
    status = HdmiAudioDeriveKsRoute(n->SymbolicLinkName, &route);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER, "hdmiaudio: cannot derive the KS route from %wZ (0x%08lX)",
              n->SymbolicLinkName, status);
        return STATUS_SUCCESS;
    }

    WdfWaitLockAcquire(h->Lock, NULL);
    if (!h->KsRouteAvailable)
    {

    }
    else if (h->Established)
    {

    }
    else if (h->SymbolicLink.Length != 0 && !h->ViaKsRoute)
    {

        kick = TRUE;
    }
    else if (h->SymbolicLink.Length != 0 &&
             RtlCompareUnicodeString(&h->SymbolicLink, &route, TRUE) == 0)
    {

        h->Retries = 0;
        kick = TRUE;
    }
    else
    {
        HdmiAudioDropTarget(h);
        RtlCopyUnicodeString(&h->SymbolicLink, &route);
        h->ViaKsRoute = TRUE;
        h->Retries = 0;
        kick = TRUE;
        Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
              "hdmiaudio: audio miniport is up (%wZ); its HDMI interface is not published, routing through %wZ",
              n->SymbolicLinkName, &h->SymbolicLink);
    }
    WdfWaitLockRelease(h->Lock);

    if (kick)
    {
        WdfWorkItemEnqueue(h->WorkItem);
    }
    return STATUS_SUCCESS;
}

static
NTSTATUS
HdmiAudioFindKsRoute(
    _In_ PHDMI_AUDIO_CONTEXT H
    )

{
    PWSTR list = NULL, p;
    UNICODE_STRING link;
    NTSTATUS status;

    status = IoGetDeviceInterfaces(&GUID_KSCATEGORY_AUDIO_LOCAL, NULL, 0, &list);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    if (list == NULL)
    {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    status = STATUS_DEVICE_NOT_CONNECTED;
    for (p = list; *p != L'\0'; p += link.Length / sizeof(WCHAR) + 1)
    {
        RtlInitUnicodeString(&link, p);
        if (!HdmiAudioContainsNoCase(&link, HDMI_AUDIO_MINIPORT_TAG))
        {
            continue;
        }
        status = HdmiAudioDeriveKsRoute(&link, &H->SymbolicLink);
        if (NT_SUCCESS(status))
        {
            H->ViaKsRoute = TRUE;
            H->Retries = 0;
            Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
                  "hdmiaudio: routing through the audio miniport's KS link %wZ (HDMI interface not published)",
                  &H->SymbolicLink);
        }
        else
        {
            H->SymbolicLink.Length = 0;
        }
        break;
    }
    ExFreePool(list);
    return status;
}

static
VOID
HdmiAudioTargetGone(
    _In_ WDFIOTARGET IoTarget,
    _In_ PCSTR Why,
    _In_ BOOLEAN ForgetLink
    )
{
    PHDMI_AUDIO_CONTEXT h = HdmiAudioFromDevice((WDFDEVICE)WdfIoTargetGetDevice(IoTarget));

    WdfWaitLockAcquire(h->CallLock, NULL);
    WdfWaitLockAcquire(h->Lock, NULL);
    if (h->Target == IoTarget)
    {

        RtlZeroMemory(h->AudioTable, sizeof(h->AudioTable));
        h->Established = FALSE;
        h->ReportedPresent = FALSE;
        if (ForgetLink && h->ViaKsRoute)
        {
            h->SymbolicLink.Length = 0;
        }
        h->Retries = 0;
        Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER, "hdmiaudio: audio miniport %s - link dropped", Why);
    }
    WdfWaitLockRelease(h->Lock);
    WdfWaitLockRelease(h->CallLock);
}

NTSTATUS
HdmiAudioEvtTargetQueryRemove(
    _In_ WDFIOTARGET IoTarget
    )
{
    HdmiAudioTargetGone(IoTarget, "is being removed (query-remove)", FALSE);
    WdfIoTargetCloseForQueryRemove(IoTarget);
    return STATUS_SUCCESS;
}

VOID
HdmiAudioEvtTargetRemoveCanceled(
    _In_ WDFIOTARGET IoTarget
    )
{
    PHDMI_AUDIO_CONTEXT h = HdmiAudioFromDevice((WDFDEVICE)WdfIoTargetGetDevice(IoTarget));

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "hdmiaudio: audio miniport removal cancelled - relinking");
    WdfWorkItemEnqueue(h->WorkItem);
}

VOID
HdmiAudioEvtTargetRemoveComplete(
    _In_ WDFIOTARGET IoTarget
    )
{
    PHDMI_AUDIO_CONTEXT h = HdmiAudioFromDevice((WDFDEVICE)WdfIoTargetGetDevice(IoTarget));

    HdmiAudioTargetGone(IoTarget, "removed", TRUE);
    WdfWorkItemEnqueue(h->WorkItem);
}

static
NTSTATUS
HdmiAudioInterfaceNotification(
    _In_ PVOID NotificationStructure,
    _Inout_opt_ PVOID Context
    )
{
    PDEVICE_INTERFACE_CHANGE_NOTIFICATION n = (PDEVICE_INTERFACE_CHANGE_NOTIFICATION)NotificationStructure;
    WDFDEVICE device = (WDFDEVICE)Context;
    PHDMI_AUDIO_CONTEXT h;

    if (device == NULL || n->SymbolicLinkName == NULL)
    {
        return STATUS_SUCCESS;
    }
    h = HdmiAudioFromDevice(device);

    WdfWaitLockAcquire(h->Lock, NULL);
    if (IsEqualGUID(&n->Event, &GUID_DEVICE_INTERFACE_ARRIVAL))
    {
        if (h->Established && h->ViaKsRoute)
        {

            Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
                  "hdmiaudio: miniport published its HDMI interface (%wZ) - already linked via the KS route, keeping it",
                  n->SymbolicLinkName);
        }
        else
        {
            if (h->SymbolicLink.Length == 0 || h->ViaKsRoute ||
                RtlCompareUnicodeString(&h->SymbolicLink, n->SymbolicLinkName, TRUE) != 0)
            {
                HdmiAudioDropTarget(h);
                RtlCopyUnicodeString(&h->SymbolicLink, n->SymbolicLinkName);
                h->ViaKsRoute = FALSE;
            }
            h->Retries = 0;
            Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "hdmiaudio: miniport HDMI interface arrived (%wZ)", &h->SymbolicLink);
        }
    }
    else if (IsEqualGUID(&n->Event, &GUID_DEVICE_INTERFACE_REMOVAL))
    {
        if (h->SymbolicLink.Length != 0 &&
            RtlCompareUnicodeString(&h->SymbolicLink, n->SymbolicLinkName, TRUE) == 0)
        {
            Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER, "hdmiaudio: miniport HDMI interface removed");
            HdmiAudioDropTarget(h);
            h->SymbolicLink.Length = 0;
        }
    }
    WdfWaitLockRelease(h->Lock);

    WdfWorkItemEnqueue(h->WorkItem);
    return STATUS_SUCCESS;
}

VOID
HdmiAudioSetSinkPresent(
    _In_ WDFDEVICE Device,
    _In_ BOOLEAN Present
    )
{
    PHDMI_AUDIO_CONTEXT h = HdmiAudioFromDevice(Device);
    BOOLEAN changed;

    if (h->Lock == NULL)
    {
        return;
    }
    WdfWaitLockAcquire(h->Lock, NULL);
    changed = (h->WantPresent != Present);
    h->WantPresent = Present;
    WdfWaitLockRelease(h->Lock);

    if (changed)
    {
        Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "hdmiaudio: sink %s", Present ? "present" : "absent");
    }
    if (h->Available)
    {
        WdfWorkItemEnqueue(h->WorkItem);
    }
}

VOID
HdmiAudioCacheEdid(
    _In_ WDFDEVICE Device,
    _In_reads_bytes_(Length) const UCHAR *Edid,
    _In_ ULONG Length
    )
{
    PHDMI_AUDIO_CONTEXT h = HdmiAudioFromDevice(Device);

    if (h->Lock == NULL || Edid == NULL || Length < HDMI_AUDIO_EDID_BLOCK)
    {
        return;
    }
    WdfWaitLockAcquire(h->Lock, NULL);
    RtlCopyMemory(h->Edid, Edid, HDMI_AUDIO_EDID_BLOCK);
    h->EdidValid = TRUE;
    WdfWaitLockRelease(h->Lock);
}

NTSTATUS
HdmiAudioCreate(
    _In_ WDFDEVICE Device
    )
{
    PHDMI_AUDIO_CONTEXT h = HdmiAudioFromDevice(Device);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_WORKITEM_CONFIG workItemConfig;
    WDF_TIMER_CONFIG timerConfig;
    NTSTATUS status;

    RtlZeroMemory(h, sizeof(*h));
    h->Enabled = TRUE;
    h->FxDevice = Device;
    h->ContainerId = GUID_HDMI_AUDIO_CONTAINER;
    RtlInitEmptyUnicodeString(&h->SymbolicLink, h->SymbolicLinkBuffer, sizeof(h->SymbolicLinkBuffer));

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;
    status = WdfWaitLockCreate(&attributes, &h->Lock);
    if (!NT_SUCCESS(status)) return status;
    status = WdfWaitLockCreate(&attributes, &h->CallLock);
    if (!NT_SUCCESS(status)) return status;

    WDF_WORKITEM_CONFIG_INIT(&workItemConfig, HdmiAudioEvtWorkItem);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;
    status = WdfWorkItemCreate(&workItemConfig, &attributes, &h->WorkItem);
    if (!NT_SUCCESS(status)) return status;

    WDF_WORKITEM_CONFIG_INIT(&workItemConfig, HdmiAudioEvtFormatWorkItem);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;
    status = WdfWorkItemCreate(&workItemConfig, &attributes, &h->FormatWorkItem);
    if (!NT_SUCCESS(status)) return status;
    h->FormatRateHz = 48000;
    h->FormatBits = 16;
    h->FormatChannels = 2;

    WDF_TIMER_CONFIG_INIT(&timerConfig, HdmiAudioEvtRetryTimer);
    timerConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfTimerCreate(&timerConfig, &attributes, &h->RetryTimer);
    if (!NT_SUCCESS(status)) return status;

    h->Available = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS
HdmiAudioPrepare(
    _In_ WDFDEVICE Device
    )
{
    PHDMI_AUDIO_CONTEXT h = HdmiAudioFromDevice(Device);
    NTSTATUS status;

    if (!h->Available)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    h->Active = TRUE;

    status = IoRegisterPlugPlayNotification(EventCategoryDeviceInterfaceChange,
                                            PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
                                            (PVOID)&GUID_QC_AUDIO_MINIPORT_HDMI,
                                            WdfDriverWdmGetDriverObject(WdfDeviceGetDriver(Device)),
                                            HdmiAudioInterfaceNotification,
                                            (PVOID)Device,
                                            &h->NotificationEntry);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "hdmiaudio: IoRegisterPlugPlayNotification failed 0x%08lX", status);
        h->NotificationEntry = NULL;
        return status;
    }

    h->KsRouteAvailable = TRUE;
    status = IoRegisterPlugPlayNotification(EventCategoryDeviceInterfaceChange,
                                            PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
                                            (PVOID)&GUID_KSCATEGORY_AUDIO_LOCAL,
                                            WdfDriverWdmGetDriverObject(WdfDeviceGetDriver(Device)),
                                            HdmiAudioKsLinkNotification,
                                            (PVOID)Device,
                                            &h->KsNotificationEntry);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER,
              "hdmiaudio: KSCATEGORY_AUDIO notification failed 0x%08lX - only the HDMI interface route is available", status);
        h->KsNotificationEntry = NULL;
        h->KsRouteAvailable = FALSE;
    }
    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "hdmiaudio: waiting for the audio miniport (HDMI interface%s)",
          h->KsRouteAvailable ? " or KS link" : "");
    return STATUS_SUCCESS;
}

VOID
HdmiAudioRelease(
    _In_ WDFDEVICE Device
    )
{
    PHDMI_AUDIO_CONTEXT h = HdmiAudioFromDevice(Device);
    NTSTATUS (*hotplug)(PVOID, BOOL) = NULL;
    NTSTATUS (*unloaded)(PVOID) = NULL;
    PVOID ctx = NULL;
    BOOLEAN reported;

    if (!h->Available)
    {
        return;
    }
    if (h->NotificationEntry != NULL)
    {
        IoUnregisterPlugPlayNotificationEx(h->NotificationEntry);
        h->NotificationEntry = NULL;
    }
    if (h->KsNotificationEntry != NULL)
    {
        IoUnregisterPlugPlayNotificationEx(h->KsNotificationEntry);
        h->KsNotificationEntry = NULL;
    }
    if (h->RetryTimer != NULL)
    {
        WdfTimerStop(h->RetryTimer, TRUE);
    }
    if (h->WorkItem != NULL)
    {
        WdfWorkItemFlush(h->WorkItem);
    }

    WdfWaitLockAcquire(h->CallLock, NULL);
    WdfWaitLockAcquire(h->Lock, NULL);
    h->Active = FALSE;
    if (h->Established)
    {
        hotplug = AUDIO(h)->Hotplug;
        unloaded = AUDIO(h)->Unloaded;
        ctx = AUDIO(h)->Context;
    }
    reported = h->ReportedPresent;
    WdfWaitLockRelease(h->Lock);

    if (hotplug != NULL && reported)
    {
        (VOID)hotplug(ctx, FALSE);
    }
    if (unloaded != NULL)
    {
        (VOID)unloaded(ctx);
    }
    WdfWaitLockRelease(h->CallLock);

    WdfWaitLockAcquire(h->Lock, NULL);
    HdmiAudioDropTarget(h);
    h->SymbolicLink.Length = 0;
    h->ViaKsRoute = FALSE;
    h->WantPresent = FALSE;
    WdfWaitLockRelease(h->Lock);
}
