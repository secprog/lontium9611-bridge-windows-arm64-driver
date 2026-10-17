/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#include "P8Cec.h"
#include <ntstrsafe.h>

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD P8EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE P8EvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE P8EvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY P8EvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT P8EvtD0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL P8ControllerEvtIoDeviceControl;
EVT_UDECX_WDF_DEVICE_QUERY_USB_CAPABILITY P8EvtQueryUsbCapability;
EVT_WDF_TIMER P8LoggingTimer;

volatile LONG g_P8CecLogLevel = 0;

static VOID P8RefreshLogging(_In_ WDFDEVICE Device)
{
    PP8_DEVICE_CONTEXT ctx = P8GetContext(Device);
    OEMBRIDGE_LOGGING_STATE logging;
    WDF_MEMORY_DESCRIPTOR output;
    WDF_REQUEST_SEND_OPTIONS options;
    ULONG_PTR returned = 0;
    NTSTATUS status;

    if (ctx->Bridge == NULL) return;
    RtlZeroMemory(&logging, sizeof(logging));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&output, &logging, sizeof(logging));
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_MS(500));
    status = WdfIoTargetSendIoctlSynchronously(ctx->Bridge, NULL,
                                               IOCTL_OEMBRIDGE_LOGGING_GET_STATE,
                                               NULL, &output, &options, &returned);
    if (NT_SUCCESS(status) && returned >= sizeof(logging) &&
        logging.Size == sizeof(logging) && OEMBridgeLoggingLevelValid(logging.Level))
    {
        InterlockedExchange(&g_P8CecLogLevel, (LONG)logging.Level);
    }
}

VOID P8LoggingTimer(_In_ WDFTIMER Timer)
{
    WDFDEVICE device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PP8_DEVICE_CONTEXT ctx = P8GetContext(device);
    KIRQL oldIrql;
    BOOLEAN active;

    KeAcquireSpinLock(&ctx->LoggingLock, &oldIrql);
    active = ctx->LoggingActive;
    KeReleaseSpinLock(&ctx->LoggingLock, oldIrql);
    if (!active) return;

    P8RefreshLogging(device);

    KeAcquireSpinLock(&ctx->LoggingLock, &oldIrql);
    if (ctx->LoggingActive) WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(1000));
    KeReleaseSpinLock(&ctx->LoggingLock, oldIrql);
}

static VOID P8StartLogging(_In_ WDFDEVICE Device)
{
    PP8_DEVICE_CONTEXT ctx = P8GetContext(Device);
    KIRQL oldIrql;

    InterlockedExchange(&g_P8CecLogLevel, OEMBRIDGE_LOG_DISABLED);
    P8RefreshLogging(Device);
    KeAcquireSpinLock(&ctx->LoggingLock, &oldIrql);
    ctx->LoggingActive = TRUE;
    WdfTimerStart(ctx->LoggingTimer, WDF_REL_TIMEOUT_IN_MS(1000));
    KeReleaseSpinLock(&ctx->LoggingLock, oldIrql);
}

static VOID P8StopLogging(_In_ WDFDEVICE Device)
{
    PP8_DEVICE_CONTEXT ctx = P8GetContext(Device);
    KIRQL oldIrql;

    KeAcquireSpinLock(&ctx->LoggingLock, &oldIrql);
    ctx->LoggingActive = FALSE;
    KeReleaseSpinLock(&ctx->LoggingLock, oldIrql);

    WdfTimerStop(ctx->LoggingTimer, TRUE);
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "DriverEntry package " P8_PACKAGE_VERSION);
    WDF_DRIVER_CONFIG_INIT(&config, P8EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                           &config, WDF_NO_HANDLE);
}

NTSTATUS
P8EvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS                     status;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES        attributes;
    UDECX_WDF_DEVICE_CONFIG      controllerConfig;
    WDF_IO_QUEUE_CONFIG          queueConfig;
    WDF_TIMER_CONFIG             timerConfig;
    WDFDEVICE                    device = NULL;
    WDFQUEUE                     queue;
    PP8_DEVICE_CONTEXT           ctx;
    ULONG                        instance;
    DECLARE_UNICODE_STRING_SIZE(name, 64);
    DECLARE_UNICODE_STRING_SIZE(link, 64);

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = P8EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = P8EvtReleaseHardware;
    pnp.EvtDeviceD0Entry         = P8EvtD0Entry;
    pnp.EvtDeviceD0Exit          = P8EvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    status = UdecxInitializeWdfDeviceInit(DeviceInit);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "UdecxInitializeWdfDeviceInit failed 0x%08X", status);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, P8_DEVICE_CONTEXT);

    for (instance = 0; instance < 32; instance++)
    {
        RtlUnicodeStringPrintf(&name, L"\\Device\\USBFDO-%u", instance);
        status = WdfDeviceInitAssignName(DeviceInit, &name);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
        if (status != STATUS_OBJECT_NAME_COLLISION)
        {
            break;
        }
    }
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "WdfDeviceCreate failed 0x%08X", status);
        return status;
    }

    ctx = P8GetContext(device);
    KeInitializeSpinLock(&ctx->CecTxLock);
    KeInitializeSpinLock(&ctx->LoggingLock);
    ctx->DefaultLa               = P8_DEVICE_TYPE_PLAYBACK;
    ctx->DeviceType              = P8_DEVICE_TYPE_PLAYBACK;
    ctx->HdmiVersion             = P8_CEC_VERSION_1_4;
    ctx->PhysicalAddressOverride = 0xFFFF;
    RtlCopyMemory(ctx->OsdName, "Rubik Pi 3", sizeof("Rubik Pi 3"));

    ctx->LineCoding[0] = 0x80; ctx->LineCoding[1] = 0x25; ctx->LineCoding[6] = 8;

    RtlUnicodeStringPrintf(&link, L"\\DosDevices\\HCD%u", instance);
    (VOID)WdfDeviceCreateSymbolicLink(device, &link);

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_USB_HOST_CONTROLLER, NULL);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "host controller interface failed 0x%08X", status);
        return status;
    }

    UDECX_WDF_DEVICE_CONFIG_INIT(&controllerConfig, P8EvtQueryUsbCapability);
    status = UdecxWdfDeviceAddUsbDeviceEmulation(device, &controllerConfig);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "UdecxWdfDeviceAddUsbDeviceEmulation failed 0x%08X", status);
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = P8ControllerEvtIoDeviceControl;
    queueConfig.PowerManaged       = WdfFalse;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "default queue failed 0x%08X", status);
        return status;
    }

    status = P8UsbInitObjects(device);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "USB objects failed 0x%08X", status);
        return status;
    }

    WDF_TIMER_CONFIG_INIT(&timerConfig, P8LoggingTimer);
    timerConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfTimerCreate(&timerConfig, &attributes, &ctx->LoggingTimer);
    if (!NT_SUCCESS(status)) return status;

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "virtual host controller USBFDO-%u ready", instance);
    return STATUS_SUCCESS;
}

NTSTATUS
P8EvtPrepareHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    status = P8BridgeOpen(Device);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER,
              "OEMBridge CEC interface not reachable (0x%08X) - adapter will report line errors", status);
    }

    return P8UsbPlugIn(Device);
}

NTSTATUS
P8EvtReleaseHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    P8StopLogging(Device);
    P8BridgeClose(Device);
    return STATUS_SUCCESS;
}

NTSTATUS
P8EvtD0Entry(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    KIRQL oldIrql;
    PP8_DEVICE_CONTEXT ctx = P8GetContext(Device);
    UNREFERENCED_PARAMETER(PreviousState);
    KeAcquireSpinLock(&ctx->TxLock, &oldIrql);
    ctx->TxOnline = TRUE;
    KeReleaseSpinLock(&ctx->TxLock, oldIrql);
    P8StartLogging(Device);
    P8BridgeStart(Device);
    P8KickTx(ctx);
    return STATUS_SUCCESS;
}

NTSTATUS
P8EvtD0Exit(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    KIRQL oldIrql;
    PP8_DEVICE_CONTEXT ctx = P8GetContext(Device);
    UNREFERENCED_PARAMETER(TargetState);
    KeAcquireSpinLock(&ctx->TxLock, &oldIrql);
    ctx->TxOnline = FALSE;
    KeReleaseSpinLock(&ctx->TxLock, oldIrql);
    P8StopLogging(Device);
    P8BridgeStop(Device);
    WdfDpcCancel(ctx->TxDpc, TRUE);
    return STATUS_SUCCESS;
}

VOID
P8ControllerEvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
    )
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(IoControlCode);

    if (UdecxWdfDeviceTryHandleUserIoctl(WdfIoQueueGetDevice(Queue), Request))
    {
        return;
    }
    WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
}

NTSTATUS
P8EvtQueryUsbCapability(
    _In_  WDFDEVICE UdecxWdfDevice,
    _In_  PGUID     CapabilityType,
    _In_  ULONG     OutputBufferLength,
    _Out_writes_to_opt_(OutputBufferLength, *ResultLength) PVOID OutputBuffer,
    _Out_ PULONG    ResultLength
    )
{
    UNREFERENCED_PARAMETER(UdecxWdfDevice);
    UNREFERENCED_PARAMETER(CapabilityType);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(OutputBuffer);

    *ResultLength = 0;
    return STATUS_NOT_SUPPORTED;
}
