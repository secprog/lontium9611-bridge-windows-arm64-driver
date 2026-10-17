/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#include "Driver.h"
#include "Device.h"
#include "Qdi.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, QdiBridgeInitialize)
#endif

static QDI_DisplayUtil_FunctionTableType g_DisplayUtil = { 0 };
static WDFDEVICE                          g_QdiDevice   = NULL;
static BOOLEAN                            g_Registered  = FALSE;

static QDI_ErrorType
QdiPanelInit(
    _In_ QDI_HandleType      hOemHandle,
    _In_ QDI_Panel_AttrType *pPanelConfig)
{
    UNREFERENCED_PARAMETER(pPanelConfig);

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "QDI: Panel_Init");

    if (hOemHandle == NULL) {
        return QDI_STATUS_FAILED;
    }

    return QDI_STATUS_OK;
}

static QDI_ErrorType
QdiPanelSetMode(
    _In_ QDI_HandleType      hOemHandle,
    _In_ QDI_Panel_AttrType *pPanelConfig)
{
    WDFDEVICE       device = (WDFDEVICE)hOemHandle;
    PDEVICE_CONTEXT ctx;

    UNREFERENCED_PARAMETER(pPanelConfig);

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "QDI: Panel_SetMode");

    if (device == NULL) {
        return QDI_STATUS_FAILED;
    }
    ctx = DeviceGetContext(device);

    WdfWaitLockAcquire(ctx->BridgeLock, NULL);
    if (ctx->BridgeReady) {
        LT9611RelockVerified(device, "QDI SetMode");
    } else {
        Trace(TRACE_LEVEL_WARNING, TRACE_DEVICE,
              "QDI: Panel_SetMode while bridge is down - D0Entry re-locks");
    }
    WdfWaitLockRelease(ctx->BridgeLock);

    return QDI_STATUS_OK;
}

static QDI_ErrorType
QdiPanelTerm(
    _In_ QDI_HandleType hOemHandle)
{
    UNREFERENCED_PARAMETER(hOemHandle);

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "QDI: Panel_Term");

    g_Registered = FALSE;
    return QDI_STATUS_OK;
}

static QDI_ErrorType
QdiPanelSetPower(
    _In_ QDI_HandleType hOemHandle,
    _In_ ULONG          bPowerOn)
{
    WDFDEVICE       device = (WDFDEVICE)hOemHandle;
    PDEVICE_CONTEXT ctx;

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
          "QDI: Panel_SetPower %u", bPowerOn);

    if (device == NULL) {
        return QDI_STATUS_FAILED;
    }
    ctx = DeviceGetContext(device);

    WdfWaitLockAcquire(ctx->BridgeLock, NULL);
    if (ctx->BridgeReady) {
        (VOID)LT9611VideoOn(device, bPowerOn ? TRUE : FALSE);
        if (bPowerOn) {
            LT9611SeedHealthState(device);
        }
    } else {
        Trace(TRACE_LEVEL_WARNING, TRACE_DEVICE,
              "QDI: Panel_SetPower while bridge is down - ignored");
    }
    WdfWaitLockRelease(ctx->BridgeLock);

    return QDI_STATUS_OK;
}

static QDI_ErrorType
QdiPanelEvent(
    _In_ QDI_HandleType hOemHandle,
    _In_ ULONG          eEvents,
    _In_ void          *pEventData,
    _In_ ULONG          uEventDataSize)
{
    WDFDEVICE device = (WDFDEVICE)hOemHandle;

    UNREFERENCED_PARAMETER(pEventData);
    UNREFERENCED_PARAMETER(uEventDataSize);

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "QDI: Panel_Event 0x%08X", eEvents);

    if (device == NULL) {
        return QDI_STATUS_FAILED;
    }

    if (eEvents & QDI_PANEL_EVENT_PRE_POWER) {
        Trace(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "QDI: PRE_POWER");
    }

    return QDI_STATUS_OK;
}

static QDI_ErrorType
QdiPanelStatus(
    _In_ QDI_HandleType hOemHandle)
{
    WDFDEVICE       device = (WDFDEVICE)hOemHandle;
    PDEVICE_CONTEXT ctx;

    if (device == NULL) {
        return QDI_STATUS_FAILED;
    }

    ctx = DeviceGetContext(device);

    return ctx->BridgeReady ? QDI_STATUS_OK : QDI_STATUS_FAILED;
}

static QDI_Panel_FunctionTableType g_PanelTable = {
    QDI_PANEL_API_VERSION,
    QdiPanelInit,
    QdiPanelSetMode,
    QdiPanelTerm,
    QdiPanelSetPower,

    NULL,
    NULL,
    QdiPanelEvent,
    QdiPanelStatus,
    NULL,
    NULL,
};

static VOID
QdiBridgeHandleIoctl(
    _In_ WDFREQUEST Request,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode)
{
    NTSTATUS       status;
    PVOID          input = NULL;
    size_t         length = 0;
    QDI_ErrorType  qdiStatus;

    if (IoControlCode != IOCTL_OEMPANEL_QUERY_INTERFACE_TABLE) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    if (WdfRequestGetRequestorMode(Request) != KernelMode) {
        WdfRequestComplete(Request, STATUS_ACCESS_DENIED);
        return;
    }

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
          "QDI: QUERY_INTERFACE_TABLE, %Iu bytes", InputBufferLength);

    if (InputBufferLength < sizeof(QDI_DisplayUtil_FunctionTableType)) {
        Trace(TRACE_LEVEL_ERROR, TRACE_DEVICE,
              "QDI: input too small (%Iu < %Iu)",
              InputBufferLength, sizeof(QDI_DisplayUtil_FunctionTableType));
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    status = WdfRequestRetrieveInputBuffer(
                Request,
                sizeof(QDI_DisplayUtil_FunctionTableType),
                &input,
                &length);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    RtlCopyMemory(&g_DisplayUtil, input, sizeof(QDI_DisplayUtil_FunctionTableType));

    if (g_DisplayUtil.QDI_DisplayUtil_Register == NULL) {
        Trace(TRACE_LEVEL_ERROR, TRACE_DEVICE, "QDI: no Register entry point");
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    qdiStatus = g_DisplayUtil.QDI_DisplayUtil_Register(
                    QDI_DISPLAY_PRIMARY,
                    (QDI_HandleType)g_QdiDevice,
                    &g_PanelTable,
                    0);

    if (qdiStatus != QDI_STATUS_OK) {
        Trace(TRACE_LEVEL_ERROR, TRACE_DEVICE,
              "QDI: Register failed %u", qdiStatus);
        WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
        return;
    }

    g_Registered = TRUE;
    Trace(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
          "QDI: registered with qcdx (version 0x%08X)", QDI_PANEL_API_VERSION);

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0);
}

static VOID
QdiEvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode)
{

    if (OEMBridgeHandleSettingsIoctl(WdfIoQueueGetDevice(Queue), Request, IoControlCode,
                                     OutputBufferLength, InputBufferLength))
    {
        return;
    }
    if (HdmiAudioHandleIoctl(WdfIoQueueGetDevice(Queue), Request, IoControlCode))
    {
        return;
    }
    if (CecHandleIoctl(WdfIoQueueGetDevice(Queue), Request, IoControlCode))
    {
        return;
    }

    QdiBridgeHandleIoctl(Request, InputBufferLength, IoControlCode);
}

NTSTATUS
QdiBridgeInitialize(
    _In_ WDFDEVICE Device)
{
    NTSTATUS              status;
    WDF_IO_QUEUE_CONFIG   queueConfig;
    WDF_OBJECT_ATTRIBUTES queueAttributes;
    WDFQUEUE              queue;

    PAGED_CODE();

    g_QdiDevice = Device;

    status = WdfDeviceCreateDeviceInterface(
                Device,
                &GUID_DEVINTERFACE_OEM_BRIDGE_DRIVER,
                NULL);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, TRACE_DEVICE,
              "QDI: CreateDeviceInterface failed 0x%08X", status);
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl         = QdiEvtIoDeviceControl;
    queueConfig.EvtIoInternalDeviceControl = QdiEvtIoDeviceControl;

    WDF_OBJECT_ATTRIBUTES_INIT(&queueAttributes);
    queueAttributes.ExecutionLevel = WdfExecutionLevelPassive;

    status = WdfIoQueueCreate(Device, &queueConfig, &queueAttributes, &queue);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, TRACE_DEVICE,
              "QDI: WdfIoQueueCreate failed 0x%08X", status);
        return status;
    }

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
          "QDI: bridge device interface published, awaiting qcdx");

    return STATUS_SUCCESS;
}
