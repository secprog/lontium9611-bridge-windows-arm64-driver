/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#include "driver.h"
#include <wdmguid.h>
#include "LpiMux.h"

#define LPI_MUX_RETRY_MS        2000
#define LPI_MUX_MAX_RETRIES     60

static const GUID GUID_ADCM_AUDIOHWRSC_INTERFACE =
    { 0x5be1e2d0, 0x4323, 0x4d19, { 0x81, 0xfe, 0x5c, 0x41, 0x85, 0xae, 0x7c, 0x05 } };

static
PLPI_MUX_CONTEXT
LpiMuxFromDevice(
    _In_ WDFDEVICE Device
    )
{
    return &DeviceGetContext(Device)->LpiMux;
}

static
VOID
LpiMuxWritePads(
    _In_ PLPI_MUX_CONTEXT M
    )
{
    ULONG pin;
    ULONG readback;
    ULONG bad = 0;

    if (M->TlmmBase == NULL || M->TlmmLength < LPASS_LPI_TLMM_LENGTH)
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER,
              "lpimux: LPI TLMM window not mapped (base %p length 0x%lX) - pads NOT muxed",
              M->TlmmBase, M->TlmmLength);
        return;
    }

    for (pin = 0; pin < LPASS_LPI_QUA_PIN_COUNT; pin++)
    {
        volatile ULONG *cfg = (volatile ULONG *)(M->TlmmBase + pin * LPASS_LPI_PIN_STRIDE);

        WRITE_REGISTER_ULONG((PULONG)&cfg[1], LPASS_LPI_QUA_VAL);
        WRITE_REGISTER_ULONG((PULONG)&cfg[0], LPASS_LPI_QUA_CFG);
        readback = READ_REGISTER_ULONG((PULONG)&cfg[0]);
        if (readback != LPASS_LPI_QUA_CFG)
        {
            bad++;
            Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                  "lpimux: gpio%lu cfg readback 0x%lX != 0x%X", pin, readback, LPASS_LPI_QUA_CFG);
        }
    }

    M->MuxApplied = (bad == 0);
    Trace(bad ? TRACE_LEVEL_WARNING : TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
          "lpimux: qua_mi2s gpio0..2 muxed for LT9611 HDMI audio (cfg 0x%X, %lu readback error(s))",
          LPASS_LPI_QUA_CFG, bad);
}

EVT_WDF_IO_TARGET_QUERY_REMOVE    LpiMuxEvtTargetQueryRemove;
EVT_WDF_IO_TARGET_REMOVE_CANCELED LpiMuxEvtTargetRemoveCanceled;
EVT_WDF_IO_TARGET_REMOVE_COMPLETE LpiMuxEvtTargetRemoveComplete;

static
VOID
LpiMuxDropInterface(
    _In_ PLPI_MUX_CONTEXT M,
    _In_ BOOLEAN TargetAlive
    )

{
    if (M->InterfaceAcquired)
    {
        if (TargetAlive && M->Interface.InterfaceHeader.InterfaceDereference != NULL)
        {
            M->Interface.InterfaceHeader.InterfaceDereference(M->Interface.InterfaceHeader.Context);
        }
        M->InterfaceAcquired = FALSE;
    }

    RtlZeroMemory(&M->Interface, sizeof(M->Interface));

    if (M->Target != NULL)
    {
        WdfIoTargetClose(M->Target);
        WdfObjectDelete(M->Target);
        M->Target = NULL;
        M->TargetGone = FALSE;
    }

    M->ClockEnabled = FALSE;
    M->MuxApplied = FALSE;
}

static
NTSTATUS
LpiMuxAcquireInterface(
    _In_ PLPI_MUX_CONTEXT M
    )
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_TARGET_OPEN_PARAMS openParams;

    if (M->InterfaceAcquired)
    {
        return STATUS_SUCCESS;
    }
    if (M->SymbolicLink.Length == 0)
    {
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = M->FxDevice;
    status = WdfIoTargetCreate(M->FxDevice, &attributes, &M->Target);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "lpimux: WdfIoTargetCreate failed 0x%08lX", status);
        M->Target = NULL;
        return status;
    }

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&openParams, &M->SymbolicLink, GENERIC_READ | GENERIC_WRITE);
    openParams.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;

    openParams.EvtIoTargetQueryRemove    = LpiMuxEvtTargetQueryRemove;
    openParams.EvtIoTargetRemoveCanceled = LpiMuxEvtTargetRemoveCanceled;
    openParams.EvtIoTargetRemoveComplete = LpiMuxEvtTargetRemoveComplete;
    status = WdfIoTargetOpen(M->Target, &openParams);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER, "lpimux: cannot open ADCM interface %wZ (0x%08lX)",
              &M->SymbolicLink, status);
        WdfObjectDelete(M->Target);
        M->Target = NULL;
        return status;
    }

    RtlZeroMemory(&M->Interface, sizeof(M->Interface));
    status = WdfIoTargetQueryForInterface(M->Target,
                                          &GUID_ADCM_AUDIOHWRSC_INTERFACE,
                                          &M->Interface.InterfaceHeader,
                                          sizeof(M->Interface),
                                          ADCM_AUDIOHWRSC_INTERFACE_VERSION,
                                          NULL);
    if (!NT_SUCCESS(status) || M->Interface.AudioHwRscIoctl == NULL)
    {
        Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER,
              "lpimux: AudioHwRsc query-interface failed 0x%08lX (size %u ver %u method %p)",
              status, M->Interface.InterfaceHeader.Size, M->Interface.InterfaceHeader.Version,
              M->Interface.AudioHwRscIoctl);
        if (NT_SUCCESS(status))
        {
            status = STATUS_NOINTERFACE;
        }
        WdfIoTargetClose(M->Target);
        WdfObjectDelete(M->Target);
        M->Target = NULL;
        return status;
    }

    M->InterfaceAcquired = TRUE;
    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "lpimux: ADCM AudioHwRsc interface acquired (%wZ)",
          &M->SymbolicLink);
    return STATUS_SUCCESS;
}

static
NTSTATUS
LpiMuxClockRequest(
    _In_ PLPI_MUX_CONTEXT M,
    _In_ BOOLEAN Enable
    )
{
    NTSTATUS status;
    NTSTATUS transportStatus;
    ULONG dspError;
    union
    {
        ADCM_HW_CLK_REQUEST Request;
        ADCM_HW_CLK_RELEASE Release;
    } payload;
    ADCM_HW_RSC_DESCRIPTOR descriptor;

    RtlZeroMemory(&payload, sizeof(payload));
    if (Enable)
    {
        payload.Request.ParamId = PARAM_ID_RSC_AUDIO_HW_CLK;
        payload.Request.ParamSize = sizeof(payload.Request) - FIELD_OFFSET(ADCM_HW_CLK_REQUEST, NumClockId);
        payload.Request.NumClockId = 1;
        payload.Request.ClockId = LPASS_CLOCK_ID_MCLK_1;
        payload.Request.ClockFreqHz = LPASS_CLOCK_MCLK_1_HZ;
        payload.Request.ClockAttribute = LPASS_CLOCK_ATTRIBUTE_COUPLE_NO;
        payload.Request.ClockRoot = LPASS_CLOCK_ROOT_DEFAULT;
        descriptor.Payload = &payload.Request;
        descriptor.PayloadSize = sizeof(payload.Request);
    }
    else
    {
        payload.Release.ParamId = PARAM_ID_RSC_AUDIO_HW_CLK;
        payload.Release.ParamSize = sizeof(payload.Release) - FIELD_OFFSET(ADCM_HW_CLK_RELEASE, NumClockId);
        payload.Release.NumClockId = 1;
        payload.Release.ClockId = LPASS_CLOCK_ID_MCLK_1;
        descriptor.Payload = &payload.Release;
        descriptor.PayloadSize = sizeof(payload.Release);
    }
    descriptor.Reserved = 0;

    transportStatus = M->Interface.AudioHwRscIoctl(
        Enable ? ADCM_AUDIOHWRSC_CMD_ENABLE_CLOCK : ADCM_AUDIOHWRSC_CMD_DISABLE_CLOCK,
        &descriptor);

    dspError = Enable ? payload.Request.ErrorCode : payload.Release.ErrorCode;
    M->LastDspError = dspError;
    status = transportStatus;
    if (NT_SUCCESS(transportStatus) && dspError != 0)
    {
        status = STATUS_UNSUCCESSFUL;
    }

    Trace(NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_WARNING, TRACE_DRIVER,
          "lpimux: %s clock 0x%X @ %u Hz -> transport 0x%08lX, DSP 0x%08lX, final 0x%08lX",
          Enable ? "ENABLE" : "DISABLE", LPASS_CLOCK_ID_MCLK_1, LPASS_CLOCK_MCLK_1_HZ,
          transportStatus, dspError, status);
    return status;
}

NTSTATUS
LpiMuxEvtTargetQueryRemove(
    _In_ WDFIOTARGET IoTarget
    )
{
    PLPI_MUX_CONTEXT m = LpiMuxFromDevice((WDFDEVICE)WdfIoTargetGetDevice(IoTarget));

    WdfWaitLockAcquire(m->Lock, NULL);
    if (m->Target == IoTarget)
    {
        if (m->ClockEnabled && m->InterfaceAcquired)
        {
            (VOID)LpiMuxClockRequest(m, FALSE);
        }
        if (m->InterfaceAcquired && m->Interface.InterfaceHeader.InterfaceDereference != NULL)
        {
            m->Interface.InterfaceHeader.InterfaceDereference(m->Interface.InterfaceHeader.Context);
        }
        m->InterfaceAcquired = FALSE;
        RtlZeroMemory(&m->Interface, sizeof(m->Interface));
        m->ClockEnabled = FALSE;
        m->MuxApplied = FALSE;
        m->TargetGone = TRUE;
        Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER,
              "lpimux: ADCM is being removed (query-remove) - vote and interface released");
    }
    WdfWaitLockRelease(m->Lock);

    WdfIoTargetCloseForQueryRemove(IoTarget);
    return STATUS_SUCCESS;
}

VOID
LpiMuxEvtTargetRemoveCanceled(
    _In_ WDFIOTARGET IoTarget
    )
{
    PLPI_MUX_CONTEXT m = LpiMuxFromDevice((WDFDEVICE)WdfIoTargetGetDevice(IoTarget));

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "lpimux: ADCM removal cancelled - re-acquiring");
    WdfWorkItemEnqueue(m->WorkItem);
}

VOID
LpiMuxEvtTargetRemoveComplete(
    _In_ WDFIOTARGET IoTarget
    )
{
    PLPI_MUX_CONTEXT m = LpiMuxFromDevice((WDFDEVICE)WdfIoTargetGetDevice(IoTarget));

    WdfWaitLockAcquire(m->Lock, NULL);
    if (m->Target == IoTarget)
    {

        m->InterfaceAcquired = FALSE;
        RtlZeroMemory(&m->Interface, sizeof(m->Interface));
        m->ClockEnabled = FALSE;
        m->MuxApplied = FALSE;
        m->TargetGone = TRUE;
        Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER, "lpimux: ADCM removed - ADSP session gone, mux re-armed");
    }
    WdfWaitLockRelease(m->Lock);

    WdfWorkItemEnqueue(m->WorkItem);
}

static
VOID
LpiMuxReconcile(
    _In_ PLPI_MUX_CONTEXT M
    )

{
    NTSTATUS status;
    BOOLEAN retry = FALSE;

    if (M->TargetGone && M->Target != NULL)
    {

        LpiMuxDropInterface(M, FALSE);
    }

    if (M->MuxApplied)
    {

        if (M->ClockEnabled && M->InterfaceAcquired)
        {
            if (NT_SUCCESS(LpiMuxClockRequest(M, FALSE)))
            {
                M->ClockEnabled = FALSE;
            }
        }
        return;
    }

    if (!M->InterfaceAcquired)
    {
        status = LpiMuxAcquireInterface(M);
        if (!NT_SUCCESS(status))
        {
            M->LastStatus = status;
            retry = (M->SymbolicLink.Length != 0);
        }
    }

    if (M->InterfaceAcquired && !M->ClockEnabled)
    {
        status = LpiMuxClockRequest(M, TRUE);
        M->LastStatus = status;
        if (NT_SUCCESS(status))
        {
            M->ClockEnabled = TRUE;
            M->Retries = 0;

            LpiMuxWritePads(M);

            if (NT_SUCCESS(LpiMuxClockRequest(M, FALSE)))
            {
                M->ClockEnabled = FALSE;
            }

        }
        else
        {
            retry = TRUE;
        }
    }

    if (retry)
    {
        if (M->Retries < LPI_MUX_MAX_RETRIES)
        {
            M->Retries++;
            WdfTimerStart(M->RetryTimer, WDF_REL_TIMEOUT_IN_MS(LPI_MUX_RETRY_MS));
        }
        else
        {
            Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                  "lpimux: giving up after %lu attempts (last 0x%08lX, DSP 0x%08lX) - qua_mi2s NOT muxed, HDMI audio will be silent",
                  M->Retries, M->LastStatus, M->LastDspError);
        }
    }
}

static
VOID
LpiMuxEvtWorkItem(
    _In_ WDFWORKITEM WorkItem
    )
{
    PLPI_MUX_CONTEXT m = LpiMuxFromDevice((WDFDEVICE)WdfWorkItemGetParentObject(WorkItem));

    WdfWaitLockAcquire(m->Lock, NULL);
    LpiMuxReconcile(m);
    WdfWaitLockRelease(m->Lock);
}

static
VOID
LpiMuxEvtRetryTimer(
    _In_ WDFTIMER Timer
    )
{
    PLPI_MUX_CONTEXT m = LpiMuxFromDevice((WDFDEVICE)WdfTimerGetParentObject(Timer));
    WdfWorkItemEnqueue(m->WorkItem);
}

static
NTSTATUS
LpiMuxInterfaceNotification(
    _In_ PVOID NotificationStructure,
    _Inout_opt_ PVOID Context
    )
{
    PDEVICE_INTERFACE_CHANGE_NOTIFICATION notification = (PDEVICE_INTERFACE_CHANGE_NOTIFICATION)NotificationStructure;
    WDFDEVICE device = (WDFDEVICE)Context;
    PLPI_MUX_CONTEXT m;

    if (device == NULL || notification->SymbolicLinkName == NULL)
    {
        return STATUS_SUCCESS;
    }
    m = LpiMuxFromDevice(device);

    WdfWaitLockAcquire(m->Lock, NULL);
    if (IsEqualGUID(&notification->Event, &GUID_DEVICE_INTERFACE_ARRIVAL))
    {

        if (m->SymbolicLink.Length == 0 ||
            RtlCompareUnicodeString(&m->SymbolicLink, notification->SymbolicLinkName, TRUE) != 0)
        {
            LpiMuxDropInterface(m, FALSE);
            RtlCopyUnicodeString(&m->SymbolicLink, notification->SymbolicLinkName);
        }
        m->Retries = 0;
        Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "lpimux: ADCM interface arrived");
    }
    else if (IsEqualGUID(&notification->Event, &GUID_DEVICE_INTERFACE_REMOVAL))
    {
        if (m->SymbolicLink.Length != 0 &&
            RtlCompareUnicodeString(&m->SymbolicLink, notification->SymbolicLinkName, TRUE) == 0)
        {

            Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER, "lpimux: ADCM interface removed (deferred notice) - forgotten");
            LpiMuxDropInterface(m, FALSE);
            m->SymbolicLink.Length = 0;
        }
    }
    WdfWaitLockRelease(m->Lock);

    WdfWorkItemEnqueue(m->WorkItem);
    return STATUS_SUCCESS;
}

NTSTATUS
LpiMuxCreate(
    _In_ WDFDEVICE Device
    )
{
    PLPI_MUX_CONTEXT m = LpiMuxFromDevice(Device);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_WORKITEM_CONFIG workItemConfig;
    WDF_TIMER_CONFIG timerConfig;
    NTSTATUS status;

    RtlZeroMemory(m, sizeof(*m));
    m->FxDevice = Device;
    RtlInitEmptyUnicodeString(&m->SymbolicLink, m->SymbolicLinkBuffer, sizeof(m->SymbolicLinkBuffer));

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;
    status = WdfWaitLockCreate(&attributes, &m->Lock);
    if (!NT_SUCCESS(status)) return status;

    WDF_WORKITEM_CONFIG_INIT(&workItemConfig, LpiMuxEvtWorkItem);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;
    status = WdfWorkItemCreate(&workItemConfig, &attributes, &m->WorkItem);
    if (!NT_SUCCESS(status)) return status;

    WDF_TIMER_CONFIG_INIT(&timerConfig, LpiMuxEvtRetryTimer);
    timerConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfTimerCreate(&timerConfig, &attributes, &m->RetryTimer);
    if (!NT_SUCCESS(status)) return status;

    return STATUS_SUCCESS;
}

NTSTATUS
LpiMuxPrepare(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PLPI_MUX_CONTEXT m = LpiMuxFromDevice(Device);
    PHYSICAL_ADDRESS pa;
    ULONG length = LPASS_LPI_TLMM_LENGTH;
    ULONG i, n;
    NTSTATUS status;

    if (m->Lock == NULL)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    pa.QuadPart = 0;
    n = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < n; i++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (r->Type == CmResourceTypeMemory &&
            r->u.Memory.Start.QuadPart == (LONGLONG)LPASS_LPI_TLMM_PA &&
            r->u.Memory.Length >= LPASS_LPI_TLMM_LENGTH)
        {
            pa = r->u.Memory.Start;
            length = r->u.Memory.Length;
            break;
        }
    }
    if (pa.QuadPart == 0)
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER,
              "lpimux: no Memory32Fixed(0x%I64X, 0x%X) resource assigned - DSDT lacks the LPI window on LT96; HDMI audio will be silent",
              (ULONGLONG)LPASS_LPI_TLMM_PA, LPASS_LPI_TLMM_LENGTH);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    m->TlmmBase = (volatile UCHAR *)MmMapIoSpaceEx(pa, length, PAGE_READWRITE | PAGE_NOCACHE);
    if (m->TlmmBase == NULL)
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "lpimux: MmMapIoSpaceEx(0x%I64X, 0x%lX) failed", pa.QuadPart, length);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    m->TlmmLength = length;
    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "lpimux: LPI TLMM 0x%I64X (PnP resource) mapped at %p",
          pa.QuadPart, m->TlmmBase);

    status = IoRegisterPlugPlayNotification(EventCategoryDeviceInterfaceChange,
                                            PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
                                            (PVOID)&GUID_ADCM_AUDIOHWRSC_INTERFACE,
                                            WdfDriverWdmGetDriverObject(WdfDeviceGetDriver(Device)),
                                            LpiMuxInterfaceNotification,
                                            (PVOID)Device,
                                            &m->NotificationEntry);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "lpimux: IoRegisterPlugPlayNotification failed 0x%08lX", status);
        m->NotificationEntry = NULL;
        return status;
    }
    return STATUS_SUCCESS;
}

VOID
LpiMuxRelease(
    _In_ WDFDEVICE Device
    )
{
    PLPI_MUX_CONTEXT m = LpiMuxFromDevice(Device);

    if (m->Lock == NULL)
    {
        return;
    }

    if (m->NotificationEntry != NULL)
    {
        IoUnregisterPlugPlayNotificationEx(m->NotificationEntry);
        m->NotificationEntry = NULL;
    }
    if (m->RetryTimer != NULL)
    {
        WdfTimerStop(m->RetryTimer, TRUE);
    }
    if (m->WorkItem != NULL)
    {
        WdfWorkItemFlush(m->WorkItem);
    }

    WdfWaitLockAcquire(m->Lock, NULL);
    if (m->ClockEnabled && m->InterfaceAcquired)
    {
        if (!NT_SUCCESS(LpiMuxClockRequest(m, FALSE)))
        {
            Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER, "lpimux: final clock release failed (DSP 0x%08lX)", m->LastDspError);
        }
        m->ClockEnabled = FALSE;
    }
    LpiMuxDropInterface(m, TRUE);
    m->SymbolicLink.Length = 0;
    if (m->TlmmBase != NULL)
    {
        MmUnmapIoSpace((PVOID)m->TlmmBase, m->TlmmLength);
        m->TlmmBase = NULL;
        m->TlmmLength = 0;
    }
    WdfWaitLockRelease(m->Lock);
}
