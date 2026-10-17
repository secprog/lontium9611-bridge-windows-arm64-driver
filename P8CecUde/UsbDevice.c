/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#include "P8Cec.h"
#include <ntstrsafe.h>

EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL P8EvtControlUrb;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL P8EvtBulkOutUrb;
EVT_WDF_IO_QUEUE_STATE P8ReadQueueReady;
EVT_WDF_DPC P8TxDpc;
EVT_UDECX_USB_ENDPOINT_RESET P8EvtEndpointReset;
EVT_UDECX_USB_ENDPOINT_START P8EvtEndpointStart;
EVT_UDECX_USB_ENDPOINT_PURGE P8EvtEndpointPurge;
EVT_WDF_IO_QUEUE_STATE P8EndpointPurgeDone;
EVT_WDF_IO_QUEUE_IO_CANCELED_ON_QUEUE P8EvtCanceledUrb;
EVT_WDF_OBJECT_CONTEXT_CLEANUP P8QueueCleanup;
EVT_UDECX_USB_DEVICE_DEFAULT_ENDPOINT_ADD P8EvtDefaultEndpointAdd;
EVT_UDECX_USB_DEVICE_ENDPOINT_ADD P8EvtEndpointAdd;
EVT_UDECX_USB_DEVICE_ENDPOINTS_CONFIGURE P8EvtEndpointsConfigure;

typedef struct _P8_USB_CONTEXT
{
    WDFDEVICE Controller;
} P8_USB_CONTEXT, *PP8_USB_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(P8_USB_CONTEXT, P8GetUsbContext)

typedef struct _P8_ENDPOINT_CONTEXT
{
    WDFDEVICE Controller;
    WDFQUEUE Queue;
    UCHAR Address;
    BOOLEAN Released;
} P8_ENDPOINT_CONTEXT, *PP8_ENDPOINT_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(P8_ENDPOINT_CONTEXT, P8GetEndpointContext)

typedef struct _P8_QUEUE_CONTEXT
{
    UDECXUSBENDPOINT Endpoint;
} P8_QUEUE_CONTEXT, *PP8_QUEUE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(P8_QUEUE_CONTEXT, P8GetQueueContext)

static WDFQUEUE *P8QueueSlot(PP8_DEVICE_CONTEXT Ctx, UCHAR Address)
{
    switch (Address)
    {
    case 0:              return &Ctx->ControlQueue;
    case P8_EP_BULK_OUT: return &Ctx->BulkOutQueue;
    case P8_EP_BULK_IN:  return &Ctx->ReadQueue;
    case P8_EP_INT_IN:   return &Ctx->IntQueue;
    default:            return NULL;
    }
}

static VOID P8DetachEndpoint(PP8_ENDPOINT_CONTEXT Ep, BOOLEAN Released)
{
    KIRQL oldIrql;
    PP8_DEVICE_CONTEXT ctx = P8GetContext(Ep->Controller);
    WDFQUEUE *slot;

    KeAcquireSpinLock(&ctx->TxLock, &oldIrql);
    slot = P8QueueSlot(ctx, Ep->Address);
    if (slot != NULL && *slot == Ep->Queue)
    {
        *slot = NULL;
    }
    if (Released)
    {
        Ep->Released = TRUE;
    }
    KeReleaseSpinLock(&ctx->TxLock, oldIrql);
}
EVT_UDECX_USB_DEVICE_D0_ENTRY P8EvtUsbLinkPowerEntry;
EVT_UDECX_USB_DEVICE_D0_EXIT P8EvtUsbLinkPowerExit;

static const UCHAR P8DeviceDescriptor[18] = {
    0x12, 0x01,
    0x00, 0x02,
    0x02, 0x02, 0x01,
    0x40,
    0x48, 0x25,
    0x01, 0x10,
    0x02, 0x00,
    0x01, 0x02, 0x03,
    0x01,
};

static const UCHAR P8ConfigDescriptor[67] = {
    0x09, 0x02, 0x43, 0x00, 0x02, 0x01, 0x00, 0xC0, 0x32,

    0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00,
    0x05, 0x24, 0x00, 0x10, 0x01,
    0x05, 0x24, 0x01, 0x00, 0x01,
    0x04, 0x24, 0x02, 0x02,
    0x05, 0x24, 0x06, 0x00, 0x01,
    0x07, 0x05, P8_EP_INT_IN,   0x03, 0x08, 0x00, 0x10,

    0x09, 0x04, 0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,
    0x07, 0x05, P8_EP_BULK_OUT, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, P8_EP_BULK_IN,  0x02, 0x40, 0x00, 0x00,
};

static const UCHAR P8LangDescriptor[4] = { 0x04, 0x03, 0x09, 0x04 };

DECLARE_CONST_UNICODE_STRING(P8StrManufacturer, L"Pulse-Eight");
DECLARE_CONST_UNICODE_STRING(P8StrProduct,      L"USB-CEC Adapter (LT9611 bridge)");
DECLARE_CONST_UNICODE_STRING(P8StrSerial,       L"LT9611CEC0001");

#define P8_PORT_KEY \
    L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_2548&PID_1001\\LT9611CEC0001\\Device Parameters"

#define P8_COMDB_KEY  L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\COM Name Arbiter"

#define P8_PORT_MIN   1
#define P8_PORT_MAX   9

static BOOLEAN P8ComNameTaken(ULONG Number)
{
    WCHAR             buffer[32];
    UNICODE_STRING    name;
    OBJECT_ATTRIBUTES oa;
    HANDLE            link = NULL;

    RtlStringCchPrintfW(buffer, ARRAYSIZE(buffer), L"\\GLOBAL??\\COM%u", Number);
    RtlInitUnicodeString(&name, buffer);
    InitializeObjectAttributes(&oa, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

    if (NT_SUCCESS(ZwOpenSymbolicLinkObject(&link, GENERIC_READ, &oa)))
    {
        ZwClose(link);
        return TRUE;
    }
    return FALSE;
}

static NTSTATUS P8OpenKey(PCWSTR Path, ACCESS_MASK Access, HANDLE *Key)
{
    UNICODE_STRING    path;
    OBJECT_ATTRIBUTES oa;

    RtlInitUnicodeString(&path, Path);
    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return ZwOpenKey(Key, Access, &oa);
}

static ULONG P8CurrentPortNumber(VOID)
{
    HANDLE                          key = NULL;
    UCHAR                           buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 64];
    PKEY_VALUE_PARTIAL_INFORMATION  vi = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
    UNICODE_STRING                  name;
    ULONG                           length = 0;
    ULONG                           number = 0;

    if (!NT_SUCCESS(P8OpenKey(P8_PORT_KEY, KEY_QUERY_VALUE, &key)))
    {
        return 0;
    }
    RtlInitUnicodeString(&name, L"PortName");
    if (NT_SUCCESS(ZwQueryValueKey(key, &name, KeyValuePartialInformation,
                                   vi, sizeof(buffer), &length)) &&
        vi->Type == REG_SZ)
    {
        PWCHAR s     = (PWCHAR)vi->Data;
        ULONG  chars = vi->DataLength / sizeof(WCHAR);
        ULONG  i;

        if (chars > 3 &&
            (s[0] == L'C' || s[0] == L'c') &&
            (s[1] == L'O' || s[1] == L'o') &&
            (s[2] == L'M' || s[2] == L'm'))
        {
            for (i = 3; i < chars && s[i] >= L'0' && s[i] <= L'9'; i++)
            {
                number = number * 10 + (ULONG)(s[i] - L'0');
            }
        }
    }
    ZwClose(key);
    return number;
}

static NTSTATUS P8WritePortNumber(ULONG Number)
{
    HANDLE         key = NULL;
    WCHAR          value[16];
    UNICODE_STRING name;
    size_t         chars = 0;
    NTSTATUS       status;

    status = P8OpenKey(P8_PORT_KEY, KEY_SET_VALUE, &key);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    RtlStringCchPrintfW(value, ARRAYSIZE(value), L"COM%u", Number);
    RtlStringCchLengthW(value, ARRAYSIZE(value), &chars);
    RtlInitUnicodeString(&name, L"PortName");
    status = ZwSetValueKey(key, &name, 0, REG_SZ, value,
                           (ULONG)((chars + 1) * sizeof(WCHAR)));
    ZwClose(key);
    return status;
}

static VOID P8SyncComDb(VOID)
{
    HANDLE                          key = NULL;
    UCHAR                           buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 128];
    PKEY_VALUE_PARTIAL_INFORMATION  vi = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
    UNICODE_STRING                  name;
    UCHAR                           db[32];
    ULONG                           length = 0;
    ULONG                           size = 0;
    ULONG                           n;
    ULONG                           added = 0;

    if (!NT_SUCCESS(P8OpenKey(P8_COMDB_KEY, KEY_QUERY_VALUE | KEY_SET_VALUE, &key)))
    {
        return;
    }
    RtlZeroMemory(db, sizeof(db));
    RtlInitUnicodeString(&name, L"ComDB");
    if (NT_SUCCESS(ZwQueryValueKey(key, &name, KeyValuePartialInformation,
                                   vi, sizeof(buffer), &length)) &&
        vi->Type == REG_BINARY && vi->DataLength > 0)
    {
        size = (vi->DataLength < sizeof(db)) ? vi->DataLength : sizeof(db);
        RtlCopyMemory(db, vi->Data, size);
    }

    for (n = 1; n <= sizeof(db) * 8; n++)
    {
        ULONG index = (n - 1) / 8;
        UCHAR bit   = (UCHAR)(1u << ((n - 1) % 8));

        if ((index < size) && (db[index] & bit))
        {
            continue;
        }
        if (!P8ComNameTaken(n))
        {
            continue;
        }
        db[index] |= bit;
        if (index >= size)
        {
            size = index + 1;
        }
        added++;
        Trace(TRACE_LEVEL_INFORMATION, TRACE_USB,
              "com: COM%u exists but was not reserved - claiming it for its owner", n);
    }

    if (added != 0)
    {
        (VOID)ZwSetValueKey(key, &name, 0, REG_BINARY, db, size);
    }
    ZwClose(key);
}

static VOID P8ReserveInComDb(ULONG Number)
{
    HANDLE                          key = NULL;
    UCHAR                           buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 128];
    PKEY_VALUE_PARTIAL_INFORMATION  vi = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
    UNICODE_STRING                  name;
    UCHAR                           db[32];
    ULONG                           length = 0;
    ULONG                           size = sizeof(db);
    ULONG                           index = (Number - 1) / 8;

    if (index >= sizeof(db))
    {
        return;
    }
    if (!NT_SUCCESS(P8OpenKey(P8_COMDB_KEY, KEY_QUERY_VALUE | KEY_SET_VALUE, &key)))
    {
        return;
    }
    RtlZeroMemory(db, sizeof(db));
    RtlInitUnicodeString(&name, L"ComDB");
    if (NT_SUCCESS(ZwQueryValueKey(key, &name, KeyValuePartialInformation,
                                   vi, sizeof(buffer), &length)) &&
        vi->Type == REG_BINARY && vi->DataLength > 0)
    {
        size = (vi->DataLength < sizeof(db)) ? vi->DataLength : sizeof(db);
        RtlCopyMemory(db, vi->Data, size);
        if (index >= size)
        {
            size = index + 1;
        }
    }
    db[index] |= (UCHAR)(1u << ((Number - 1) % 8));
    (VOID)ZwSetValueKey(key, &name, 0, REG_BINARY, db, size);
    ZwClose(key);
}

static VOID P8ChooseComPort(VOID)
{
    ULONG current;
    ULONG n;

    P8SyncComDb();

    current = P8CurrentPortNumber();

    if (current >= P8_PORT_MIN && current <= P8_PORT_MAX && !P8ComNameTaken(current))
    {

        Trace(TRACE_LEVEL_INFORMATION, TRACE_USB, "com: keeping COM%u", current);
        return;
    }

    for (n = P8_PORT_MIN; n <= P8_PORT_MAX; n++)
    {
        if (!P8ComNameTaken(n))
        {
            NTSTATUS status = P8WritePortNumber(n);

            if (current == 0)
            {

                Trace(TRACE_LEVEL_INFORMATION, TRACE_USB,
                      "com: no PortName yet - suggesting COM%u (0x%08X)%s", n, status,
                      (status == STATUS_OBJECT_NAME_NOT_FOUND)
                          ? " - usbser will allocate from the arbiter" : "");
            }
            else
            {
                Trace(TRACE_LEVEL_WARNING, TRACE_USB,
                      "com: COM%u is owned by another driver - moving to COM%u (0x%08X)",
                      current, n, status);
            }
            if (NT_SUCCESS(status))
            {
                P8ReserveInComDb(n);
            }
            return;
        }
    }

    Trace(TRACE_LEVEL_ERROR, TRACE_USB,
          "com: COM1-COM9 are all taken - leaving COM%u; libCEC cannot parse a two-digit port",
          current);
}

NTSTATUS P8UsbInitObjects(WDFDEVICE Device)
{
    PP8_DEVICE_CONTEXT    ctx = P8GetContext(Device);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_DPC_CONFIG        dpcConfig;
    NTSTATUS              status;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;

    KeInitializeSpinLock(&ctx->TxLock);

    WDF_DPC_CONFIG_INIT(&dpcConfig, P8TxDpc);
    dpcConfig.AutomaticSerialization = FALSE;
    status = WdfDpcCreate(&dpcConfig, &attributes, &ctx->TxDpc);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    return status;
}

static NTSTATUS
P8CreateDynamicEndpoint(
    _In_ UDECXUSBDEVICE UsbDevice,
    _Inout_ PUDECXUSBENDPOINT_INIT *Init,
    _In_ UCHAR Address
    )
{
    WDFDEVICE device = P8GetUsbContext(UsbDevice)->Controller;
    UDECX_USB_ENDPOINT_CALLBACKS callbacks;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG config;
    UDECXUSBENDPOINT endpoint;
    PP8_ENDPOINT_CONTEXT ep;
    NTSTATUS status;
    BOOLEAN manual = (Address == P8_EP_BULK_IN || Address == P8_EP_INT_IN);

    UdecxUsbEndpointInitSetEndpointAddress(*Init, Address);
    UDECX_USB_ENDPOINT_CALLBACKS_INIT(&callbacks, P8EvtEndpointReset);
    callbacks.EvtUsbEndpointStart = P8EvtEndpointStart;
    callbacks.EvtUsbEndpointPurge = P8EvtEndpointPurge;
    UdecxUsbEndpointInitSetCallbacks(*Init, &callbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, P8_ENDPOINT_CONTEXT);
    attributes.ParentObject = UsbDevice;
    status = UdecxUsbEndpointCreate(Init, &attributes, &endpoint);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    ep = P8GetEndpointContext(endpoint);
    ep->Controller = device;
    ep->Address = Address;

    WDF_IO_QUEUE_CONFIG_INIT(&config, manual ? WdfIoQueueDispatchManual : WdfIoQueueDispatchSequential);
    config.PowerManaged = WdfFalse;

    config.AllowZeroLengthRequests = TRUE;
    config.EvtIoCanceledOnQueue = P8EvtCanceledUrb;
    if (!manual)
    {
        config.EvtIoInternalDeviceControl = (Address == 0) ? P8EvtControlUrb : P8EvtBulkOutUrb;
    }
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, P8_QUEUE_CONTEXT);
    attributes.ParentObject = endpoint;
    attributes.EvtCleanupCallback = P8QueueCleanup;
    if (!manual)
    {

        attributes.ExecutionLevel = WdfExecutionLevelPassive;
    }
    status = WdfIoQueueCreate(device, &config, &attributes, &ep->Queue);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    P8GetQueueContext(ep->Queue)->Endpoint = endpoint;
    if (Address == P8_EP_BULK_IN)
    {
        status = WdfIoQueueReadyNotify(ep->Queue, P8ReadQueueReady, P8GetContext(device));
        if (!NT_SUCCESS(status))
        {
            return status;
        }
    }
    UdecxUsbEndpointSetWdfIoQueue(endpoint, ep->Queue);
    Trace(TRACE_LEVEL_INFORMATION, TRACE_USB,
          "dynamic add 0x%02X endpoint %p queue %p", Address, endpoint, ep->Queue);
    return STATUS_SUCCESS;
}

NTSTATUS P8EvtDefaultEndpointAdd(UDECXUSBDEVICE UsbDevice, PUDECXUSBENDPOINT_INIT Init)
{
    return P8CreateDynamicEndpoint(UsbDevice, &Init, 0);
}

NTSTATUS P8EvtEndpointAdd(UDECXUSBDEVICE UsbDevice, PUDECX_USB_ENDPOINT_INIT_AND_METADATA Metadata)
{
    PUSB_ENDPOINT_DESCRIPTOR d = Metadata->EndpointDescriptor;
    UCHAR expectedType;
    USHORT expectedPacket;

    if (d == NULL || Metadata->EndpointDescriptorBufferLength < sizeof(*d) ||
        d->bLength != sizeof(*d) || d->bDescriptorType != USB_ENDPOINT_DESCRIPTOR_TYPE)
    {
        return STATUS_INVALID_PARAMETER;
    }
    switch (d->bEndpointAddress)
    {
    case P8_EP_BULK_OUT:
    case P8_EP_BULK_IN:
        expectedType = USB_ENDPOINT_TYPE_BULK;
        expectedPacket = 64;
        break;
    case P8_EP_INT_IN:
        expectedType = USB_ENDPOINT_TYPE_INTERRUPT;
        expectedPacket = 8;
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }
    if ((d->bmAttributes & USB_ENDPOINT_TYPE_MASK) != expectedType ||
        d->wMaxPacketSize != expectedPacket)
    {
        return STATUS_INVALID_PARAMETER;
    }
    return P8CreateDynamicEndpoint(UsbDevice, &Metadata->UdecxUsbEndpointInit, d->bEndpointAddress);
}

VOID P8EvtEndpointsConfigure(UDECXUSBDEVICE UsbDevice, WDFREQUEST Request,
                            PUDECX_ENDPOINTS_CONFIGURE_PARAMS Params)
{
    KIRQL oldIrql;
    PP8_DEVICE_CONTEXT ctx = P8GetContext(P8GetUsbContext(UsbDevice)->Controller);
    ULONG i;

    Trace(TRACE_LEVEL_INFORMATION, TRACE_USB,
          "dynamic configure type %u config %u interface %u alt %u add %u release %u",
          Params->ConfigureType, Params->NewConfigurationValue,
          Params->InterfaceNumber, Params->NewInterfaceSetting,
          Params->EndpointsToConfigureCount, Params->ReleasedEndpointsCount);
    for (i = 0; i < Params->ReleasedEndpointsCount; i++)
    {
        PP8_ENDPOINT_CONTEXT ep = P8GetEndpointContext(Params->ReleasedEndpoints[i]);
        P8DetachEndpoint(ep, TRUE);
        Trace(TRACE_LEVEL_INFORMATION, TRACE_USB, "dynamic release 0x%02X", ep->Address);

    }
    if (Params->ConfigureType == UdecxEndpointsConfigureTypeDeviceConfigurationChange)
    {
        ctx->ConfigurationValue = Params->NewConfigurationValue;
    }
    for (i = 0; i < Params->EndpointsToConfigureCount; i++)
    {
        PP8_ENDPOINT_CONTEXT ep = P8GetEndpointContext(Params->EndpointsToConfigure[i]);
        WDFQUEUE *slot;

        KeAcquireSpinLock(&ctx->TxLock, &oldIrql);
        slot = P8QueueSlot(ctx, ep->Address);
        if (!ep->Released && slot != NULL)
        {
            *slot = ep->Queue;
        }
        KeReleaseSpinLock(&ctx->TxLock, oldIrql);
        Trace(TRACE_LEVEL_INFORMATION, TRACE_USB,
              "dynamic configured 0x%02X queue %p", ep->Address, ep->Queue);
    }
    P8KickTx(ctx);

    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID P8QueueCleanup(WDFOBJECT Object)
{
    UDECXUSBENDPOINT endpoint = P8GetQueueContext(Object)->Endpoint;
    if (endpoint != NULL)
    {

        P8DetachEndpoint(P8GetEndpointContext(endpoint), TRUE);
    }
}

NTSTATUS P8UsbPlugIn(WDFDEVICE Device)
{
    KIRQL oldIrql;
    PP8_DEVICE_CONTEXT                      ctx = P8GetContext(Device);
    PUDECXUSBDEVICE_INIT                    init;
    UDECX_USB_DEVICE_STATE_CHANGE_CALLBACKS callbacks;
    UDECX_USB_DEVICE_PLUG_IN_OPTIONS        options;
    WDF_OBJECT_ATTRIBUTES                   attributes;
    NTSTATUS                                status;

    if (ctx->PluggedIn)
    {
        return STATUS_SUCCESS;
    }

    P8ChooseComPort();

    init = UdecxUsbDeviceInitAllocate(Device);
    if (init == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    UDECX_USB_DEVICE_CALLBACKS_INIT(&callbacks);
    callbacks.EvtUsbDeviceDefaultEndpointAdd = P8EvtDefaultEndpointAdd;
    callbacks.EvtUsbDeviceEndpointAdd = P8EvtEndpointAdd;
    callbacks.EvtUsbDeviceEndpointsConfigure = P8EvtEndpointsConfigure;
    callbacks.EvtUsbDeviceLinkPowerEntry = P8EvtUsbLinkPowerEntry;
    callbacks.EvtUsbDeviceLinkPowerExit  = P8EvtUsbLinkPowerExit;
    UdecxUsbDeviceInitSetStateChangeCallbacks(init, &callbacks);

    UdecxUsbDeviceInitSetSpeed(init, UdecxUsbFullSpeed);
    UdecxUsbDeviceInitSetEndpointsType(init, UdecxEndpointTypeDynamic);

    status = UdecxUsbDeviceInitAddDescriptor(init, (PUCHAR)P8DeviceDescriptor, sizeof(P8DeviceDescriptor));
    if (NT_SUCCESS(status))
    {
        status = UdecxUsbDeviceInitAddDescriptor(init, (PUCHAR)P8ConfigDescriptor, sizeof(P8ConfigDescriptor));
    }
    if (NT_SUCCESS(status))
    {
        status = UdecxUsbDeviceInitAddDescriptorWithIndex(init, (PUCHAR)P8LangDescriptor, sizeof(P8LangDescriptor), 0);
    }
    if (NT_SUCCESS(status))
    {
        status = UdecxUsbDeviceInitAddStringDescriptor(init, &P8StrManufacturer, 1, 0x0409);
    }
    if (NT_SUCCESS(status))
    {
        status = UdecxUsbDeviceInitAddStringDescriptor(init, &P8StrProduct, 2, 0x0409);
    }
    if (NT_SUCCESS(status))
    {
        status = UdecxUsbDeviceInitAddStringDescriptor(init, &P8StrSerial, 3, 0x0409);
    }
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_USB, "descriptor setup failed 0x%08X", status);
        UdecxUsbDeviceInitFree(init);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, P8_USB_CONTEXT);
    status = UdecxUsbDeviceCreate(&init, &attributes, &ctx->UsbDevice);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_USB, "UdecxUsbDeviceCreate failed 0x%08X", status);
        UdecxUsbDeviceInitFree(init);
        return status;
    }

    P8GetUsbContext(ctx->UsbDevice)->Controller = Device;
    UDECX_USB_DEVICE_PLUG_IN_OPTIONS_INIT(&options);
    options.Usb20PortNumber = 1;
    status = UdecxUsbDevicePlugIn(ctx->UsbDevice, &options);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_USB, "endpoint/plug-in failed 0x%08X", status);
        WdfObjectDelete(ctx->UsbDevice);
        ctx->UsbDevice = NULL;
        return status;
    }

    KeAcquireSpinLock(&ctx->TxLock, &oldIrql);
    ctx->PluggedIn = TRUE;
    KeReleaseSpinLock(&ctx->TxLock, oldIrql);
    P8KickTx(ctx);
    Trace(TRACE_LEVEL_INFORMATION, TRACE_USB,
          "plugged in: VID_%04X&PID_%04X CDC-ACM, full speed, port 1", P8_USB_VID, P8_USB_PID);

    return STATUS_SUCCESS;
}

#define CDC_SET_LINE_CODING         0x20
#define CDC_GET_LINE_CODING         0x21
#define CDC_SET_CONTROL_LINE_STATE  0x22
#define CDC_SEND_BREAK              0x23

VOID
P8EvtControlUrb(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
    )
{
    PP8_DEVICE_CONTEXT           ctx = P8GetContext(WdfIoQueueGetDevice(Queue));
    WDF_USB_CONTROL_SETUP_PACKET setup;
    PUCHAR                       buffer = NULL;
    ULONG                        length = 0;
    ULONG                        done = 0;
    USBD_STATUS                  usbd = USBD_STATUS_SUCCESS;
    NTSTATUS                     status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (IoControlCode != IOCTL_INTERNAL_USB_SUBMIT_URB)
    {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
    status = UdecxUrbRetrieveControlSetupPacket(Request, &setup);
    if (!NT_SUCCESS(status))
    {
        UdecxUrbCompleteWithNtStatus(Request, status);
        return;
    }
    if (!NT_SUCCESS(UdecxUrbRetrieveBuffer(Request, &buffer, &length)))
    {
        buffer = NULL;
        length = 0;
    }

    if (setup.Packet.bm.Request.Type == BmRequestClass)
    {
        switch (setup.Packet.bRequest)
        {
        case CDC_SET_LINE_CODING:
            done = (length < sizeof(ctx->LineCoding)) ? length : sizeof(ctx->LineCoding);
            if (buffer != NULL && done != 0)
            {
                RtlCopyMemory(ctx->LineCoding, buffer, done);
            }
            break;
        case CDC_GET_LINE_CODING:
            if (buffer != NULL && length >= sizeof(ctx->LineCoding))
            {
                RtlCopyMemory(buffer, ctx->LineCoding, sizeof(ctx->LineCoding));
                done = sizeof(ctx->LineCoding);
            }
            break;
        case CDC_SET_CONTROL_LINE_STATE:

            break;
        case CDC_SEND_BREAK:
            break;
        default:
            usbd = USBD_STATUS_STALL_PID;
            break;
        }
    }
    else if (setup.Packet.bm.Request.Type == BmRequestStandard)
    {

        switch (setup.Packet.bRequest)
        {
        case USB_REQUEST_GET_STATUS:
            if (buffer != NULL && length >= 2)
            {
                buffer[0] = 0; buffer[1] = 0;
                done = 2;
            }
            break;
        case USB_REQUEST_GET_CONFIGURATION:
            if (buffer != NULL && length >= 1)
            {
                buffer[0] = ctx->ConfigurationValue;
                done = 1;
            }
            break;
        case USB_REQUEST_GET_INTERFACE:
            if (buffer != NULL && length >= 1)
            {
                buffer[0] = 0;
                done = 1;
            }
            break;
        default:
            usbd = USBD_STATUS_STALL_PID;
            break;
        }
    }
    else
    {
        usbd = USBD_STATUS_STALL_PID;
    }

    Trace(TRACE_LEVEL_VERBOSE, TRACE_USB,
          "ep0: type %u req 0x%02X val 0x%04X idx 0x%04X len %u -> %u byte(s), usbd 0x%08X",
          setup.Packet.bm.Request.Type, setup.Packet.bRequest,
          setup.Packet.wValue.Value, setup.Packet.wIndex.Value,
          setup.Packet.wLength, done, usbd);

    UdecxUrbSetBytesCompleted(Request, done);
    UdecxUrbComplete(Request, usbd);
}

VOID
P8EvtBulkOutUrb(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
    )
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PUCHAR    buffer = NULL;
    ULONG     length = 0;
    NTSTATUS  status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (IoControlCode != IOCTL_INTERNAL_USB_SUBMIT_URB)
    {
        Trace(TRACE_LEVEL_WARNING, TRACE_P8, "bulk out: unexpected IOCTL 0x%08X", IoControlCode);
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
    status = UdecxUrbRetrieveBuffer(Request, &buffer, &length);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_P8, "bulk out: no buffer (0x%08X)", status);
        UdecxUrbCompleteWithNtStatus(Request, status);
        return;
    }

    Trace(TRACE_LEVEL_VERBOSE, TRACE_P8,
          "host->adapter %u byte(s): %02X %02X %02X %02X", length,
          (length > 0) ? buffer[0] : 0, (length > 1) ? buffer[1] : 0,
          (length > 2) ? buffer[2] : 0, (length > 3) ? buffer[3] : 0);

    P8HostBytes(device, buffer, length);

    UdecxUrbSetBytesCompleted(Request, length);
    UdecxUrbComplete(Request, USBD_STATUS_SUCCESS);
}

VOID P8KickTx(PP8_DEVICE_CONTEXT Ctx)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&Ctx->TxLock, &oldIrql);
    Ctx->TxDpcPending = TRUE;
    if (Ctx->TxOnline)
    {

        WdfDpcEnqueue(Ctx->TxDpc);
    }
    KeReleaseSpinLock(&Ctx->TxLock, oldIrql);
}

VOID P8ReadQueueReady(_In_ WDFQUEUE Queue, _In_ WDFCONTEXT Context)
{
    Trace(TRACE_LEVEL_VERBOSE, TRACE_P8, "bulk in: read queue %p ready", Queue);
    P8KickTx((PP8_DEVICE_CONTEXT)Context);
}

static VOID P8DrainTx(PP8_DEVICE_CONTEXT ctx)
{
    KIRQL oldIrql;
    for (;;)
    {
        WDFREQUEST request;
        WDFQUEUE   queue;
        NTSTATUS   status;
        PUCHAR     buffer = NULL;
        ULONG      length = 0;
        ULONG      n, i, remaining;
        BOOLEAN    ready;
        WDF_REQUEST_PARAMETERS parameters;

        KeAcquireSpinLock(&ctx->TxLock, &oldIrql);
        ready = (ctx->TxCount != 0 && ctx->PluggedIn && ctx->TxOnline) ? TRUE : FALSE;
        queue = ready ? ctx->ReadQueue : NULL;
        if (queue == NULL)
        {
            KeReleaseSpinLock(&ctx->TxLock, oldIrql);
            return;
        }

        status = WdfIoQueueRetrieveNextRequest(queue, &request);
        KeReleaseSpinLock(&ctx->TxLock, oldIrql);
        if (!NT_SUCCESS(status))
        {
            return;
        }
        WDF_REQUEST_PARAMETERS_INIT(&parameters);
        WdfRequestGetParameters(request, &parameters);
        if (parameters.Type != WdfRequestTypeDeviceControlInternal ||
            parameters.Parameters.DeviceIoControl.IoControlCode != IOCTL_INTERNAL_USB_SUBMIT_URB)
        {
            WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST);
            continue;
        }
        if (!NT_SUCCESS(UdecxUrbRetrieveBuffer(request, &buffer, &length)) || length == 0)
        {
            UdecxUrbCompleteWithNtStatus(request, STATUS_INVALID_PARAMETER);
            continue;
        }

        KeAcquireSpinLock(&ctx->TxLock, &oldIrql);
        n = (length < ctx->TxCount) ? length : ctx->TxCount;
        for (i = 0; i < n; i++)
        {
            buffer[i]   = ctx->TxFifo[ctx->TxHead];
            ctx->TxHead = (ctx->TxHead + 1) % P8_TX_FIFO_SIZE;
        }
        ctx->TxCount -= n;
        remaining = ctx->TxCount;
        KeReleaseSpinLock(&ctx->TxLock, oldIrql);

        Trace(TRACE_LEVEL_VERBOSE, TRACE_P8,
              "adapter->host %u byte(s) delivered (%u still queued)", n, remaining);

        UdecxUrbSetBytesCompleted(request, n);
        UdecxUrbComplete(request, USBD_STATUS_SUCCESS);
    }
}

VOID P8TxDpc(WDFDPC Dpc)
{
    KIRQL oldIrql;
    PP8_DEVICE_CONTEXT ctx = P8GetContext((WDFDEVICE)WdfDpcGetParentObject(Dpc));

    KeAcquireSpinLock(&ctx->TxLock, &oldIrql);
    if (ctx->TxDpcActive)
    {
        KeReleaseSpinLock(&ctx->TxLock, oldIrql);
        return;
    }
    ctx->TxDpcActive = TRUE;
    do
    {
        ctx->TxDpcPending = FALSE;
        KeReleaseSpinLock(&ctx->TxLock, oldIrql);
        P8DrainTx(ctx);
        KeAcquireSpinLock(&ctx->TxLock, &oldIrql);
    } while (ctx->TxDpcPending);
    ctx->TxDpcActive = FALSE;
    KeReleaseSpinLock(&ctx->TxLock, oldIrql);
}

VOID
P8EvtEndpointReset(
    _In_ UDECXUSBENDPOINT UdecxUsbEndpoint,
    _In_ WDFREQUEST       Request
    )
{
    UNREFERENCED_PARAMETER(UdecxUsbEndpoint);
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID P8EvtEndpointStart(UDECXUSBENDPOINT Endpoint)
{
    KIRQL oldIrql;
    PP8_ENDPOINT_CONTEXT ep = P8GetEndpointContext(Endpoint);
    PP8_DEVICE_CONTEXT ctx = P8GetContext(ep->Controller);
    WDFQUEUE *slot;

    KeAcquireSpinLock(&ctx->TxLock, &oldIrql);
    if (ep->Released)
    {
        KeReleaseSpinLock(&ctx->TxLock, oldIrql);
        Trace(TRACE_LEVEL_ERROR, TRACE_USB, "start of released endpoint 0x%02X", ep->Address);
        return;
    }
    KeReleaseSpinLock(&ctx->TxLock, oldIrql);
    WdfIoQueueStart(ep->Queue);
    KeAcquireSpinLock(&ctx->TxLock, &oldIrql);
    slot = P8QueueSlot(ctx, ep->Address);
    if (!ep->Released && slot != NULL)
    {
        *slot = ep->Queue;
    }
    KeReleaseSpinLock(&ctx->TxLock, oldIrql);
    Trace(TRACE_LEVEL_INFORMATION, TRACE_USB, "dynamic start 0x%02X queue %p", ep->Address, ep->Queue);
    P8KickTx(ctx);
}

VOID P8EvtEndpointPurge(UDECXUSBENDPOINT Endpoint)
{
    PP8_ENDPOINT_CONTEXT ep = P8GetEndpointContext(Endpoint);
    P8DetachEndpoint(ep, FALSE);
    Trace(TRACE_LEVEL_INFORMATION, TRACE_USB, "dynamic purge 0x%02X", ep->Address);

    WdfIoQueuePurge(ep->Queue, P8EndpointPurgeDone, Endpoint);
}

VOID P8EndpointPurgeDone(WDFQUEUE Queue, WDFCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Queue);
    Trace(TRACE_LEVEL_INFORMATION, TRACE_USB, "dynamic purge done 0x%02X",
          P8GetEndpointContext((UDECXUSBENDPOINT)Context)->Address);
    UdecxUsbEndpointPurgeComplete((UDECXUSBENDPOINT)Context);
}

VOID P8EvtCanceledUrb(WDFQUEUE Queue, WDFREQUEST Request)
{
    UNREFERENCED_PARAMETER(Queue);
    UdecxUrbCompleteWithNtStatus(Request, STATUS_CANCELLED);
}

NTSTATUS
P8EvtUsbLinkPowerEntry(
    _In_ WDFDEVICE      UdecxWdfDevice,
    _In_ UDECXUSBDEVICE UdecxUsbDevice
    )
{
    UNREFERENCED_PARAMETER(UdecxWdfDevice);
    UNREFERENCED_PARAMETER(UdecxUsbDevice);
    return STATUS_SUCCESS;
}

NTSTATUS
P8EvtUsbLinkPowerExit(
    _In_ WDFDEVICE                     UdecxWdfDevice,
    _In_ UDECXUSBDEVICE                UdecxUsbDevice,
    _In_ UDECX_USB_DEVICE_WAKE_SETTING WakeSetting
    )
{
    UNREFERENCED_PARAMETER(UdecxWdfDevice);
    UNREFERENCED_PARAMETER(UdecxUsbDevice);
    UNREFERENCED_PARAMETER(WakeSetting);
    return STATUS_SUCCESS;
}
