/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <usb.h>
#include <usbdlib.h>
#include <usbioctl.h>
#include <wdfusb.h>
#include <udecx.h>
#include "..\OEMBridge\OEMBridgeIoctl.h"
#include "Trace.h"
#include "..\Version.h"

#define P8_PACKAGE_VERSION LT9611_VERSION_STRING

#define P8_MSGSTART                 0xFF
#define P8_MSGEND                   0xFE
#define P8_MSGESC                   0xFD
#define P8_ESCOFFSET                3

#define P8_FRAME_EOM                0x80
#define P8_FRAME_ACK                0x40

enum
{
    MSGCODE_NOTHING = 0,
    MSGCODE_PING,
    MSGCODE_TIMEOUT_ERROR,
    MSGCODE_HIGH_ERROR,
    MSGCODE_LOW_ERROR,
    MSGCODE_FRAME_START,
    MSGCODE_FRAME_DATA,
    MSGCODE_RECEIVE_FAILED,
    MSGCODE_COMMAND_ACCEPTED,
    MSGCODE_COMMAND_REJECTED,
    MSGCODE_SET_ACK_MASK,
    MSGCODE_TRANSMIT,
    MSGCODE_TRANSMIT_EOM,
    MSGCODE_TRANSMIT_IDLETIME,
    MSGCODE_TRANSMIT_ACK_POLARITY,
    MSGCODE_TRANSMIT_LINE_TIMEOUT,
    MSGCODE_TRANSMIT_SUCCEEDED,
    MSGCODE_TRANSMIT_FAILED_LINE,
    MSGCODE_TRANSMIT_FAILED_ACK,
    MSGCODE_TRANSMIT_FAILED_TIMEOUT_DATA,
    MSGCODE_TRANSMIT_FAILED_TIMEOUT_LINE,
    MSGCODE_FIRMWARE_VERSION,
    MSGCODE_START_BOOTLOADER,
    MSGCODE_GET_BUILDDATE,
    MSGCODE_SET_CONTROLLED,
    MSGCODE_GET_AUTO_ENABLED,
    MSGCODE_SET_AUTO_ENABLED,
    MSGCODE_GET_DEFAULT_LOGICAL_ADDRESS,
    MSGCODE_SET_DEFAULT_LOGICAL_ADDRESS,
    MSGCODE_GET_LOGICAL_ADDRESS_MASK,
    MSGCODE_SET_LOGICAL_ADDRESS_MASK,
    MSGCODE_GET_PHYSICAL_ADDRESS,
    MSGCODE_SET_PHYSICAL_ADDRESS,
    MSGCODE_GET_DEVICE_TYPE,
    MSGCODE_SET_DEVICE_TYPE,
    MSGCODE_GET_HDMI_VERSION,
    MSGCODE_SET_HDMI_VERSION,
    MSGCODE_GET_OSD_NAME,
    MSGCODE_SET_OSD_NAME,
    MSGCODE_WRITE_EEPROM,
    MSGCODE_GET_ADAPTER_TYPE,
    MSGCODE_SET_ACTIVE_SOURCE,
    MSGCODE_GET_AUTO_POWER_ON,
    MSGCODE_SET_AUTO_POWER_ON,
};

#define P8_FIRMWARE_VERSION         9
#define P8_ADAPTERTYPE_EXTERNAL     1
#define P8_CEC_VERSION_1_4          5
#define P8_DEVICE_TYPE_PLAYBACK     4
#define P8_LA_UNREGISTERED          15

#define P8_USB_VID                  0x2548
#define P8_USB_PID                  0x1001

#define P8_EP_INT_IN                0x81
#define P8_EP_BULK_OUT              0x02
#define P8_EP_BULK_IN               0x82

#define P8_TX_FIFO_SIZE             4096
#define P8_RX_MSG_MAX               64
#define P8_CEC_TX_QUEUE_SIZE        16

typedef struct _P8_DEVICE_CONTEXT
{

    UDECXUSBDEVICE      UsbDevice;

    WDFQUEUE            ControlQueue;
    WDFQUEUE            BulkOutQueue;
    WDFQUEUE            ReadQueue;
    WDFQUEUE            IntQueue;
    BOOLEAN             PluggedIn;
    BOOLEAN             TxOnline;
    UCHAR               ConfigurationValue;
    UCHAR               LineCoding[7];

    KSPIN_LOCK          TxLock;
    UCHAR               TxFifo[P8_TX_FIFO_SIZE];
    ULONG               TxHead;
    ULONG               TxCount;
    ULONG               TxDropped;
    WDFDPC              TxDpc;
    BOOLEAN             TxDpcActive;
    BOOLEAN             TxDpcPending;

    BOOLEAN             RxStarted;
    BOOLEAN             RxEscaped;
    UCHAR               RxMsg[P8_RX_MSG_MAX];
    ULONG               RxLen;

    USHORT              AckMask;
    UCHAR               AutoEnabled;
    UCHAR               DefaultLa;
    UCHAR               DeviceType;
    UCHAR               HdmiVersion;
    USHORT              PhysicalAddressOverride;
    CHAR                OsdName[14];

    UCHAR               TxFrame[OEMBRIDGE_CEC_MAX_FRAME];
    UCHAR               TxFrameLen;

    KSPIN_LOCK          CecTxLock;
    OEMBRIDGE_CEC_FRAME CecTxQueue[P8_CEC_TX_QUEUE_SIZE];
    ULONG               CecTxHead;
    ULONG               CecTxCount;
    ULONG               CecTxRejected;
    ULONG               CecTxSequence;
    WDFTIMER            CecTxTimer;

    WDFIOTARGET         Bridge;
    BOOLEAN             Running;
    BOOLEAN             RxPosted;
    BOOLEAN             CecTxInFlight;
    WDFREQUEST          RxRequest;
    WDFMEMORY           RxMemory;
    OEMBRIDGE_CEC_FRAME *RxBuffer;
    WDFREQUEST          TxRequest;
    WDFMEMORY           TxMemory;
    OEMBRIDGE_CEC_FRAME *TxBuffer;
    WDFTIMER            RetryTimer;

    WDFTIMER            LoggingTimer;
    KSPIN_LOCK          LoggingLock;
    BOOLEAN             LoggingActive;
} P8_DEVICE_CONTEXT, *PP8_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(P8_DEVICE_CONTEXT, P8GetContext)

NTSTATUS P8UsbInitObjects(_In_ WDFDEVICE Device);
NTSTATUS P8UsbPlugIn(_In_ WDFDEVICE Device);
VOID     P8KickTx(_In_ PP8_DEVICE_CONTEXT Ctx);

NTSTATUS P8BridgeOpen(_In_ WDFDEVICE Device);
VOID     P8BridgeClose(_In_ WDFDEVICE Device);
VOID     P8BridgeStart(_In_ WDFDEVICE Device);
VOID     P8BridgeStop(_In_ WDFDEVICE Device);
VOID     P8HostBytes(_In_ WDFDEVICE Device, _In_reads_(Length) const UCHAR *Bytes, _In_ ULONG Length);
VOID     P8SendMsg(_In_ PP8_DEVICE_CONTEXT Ctx, _In_ UCHAR Code, _In_reads_opt_(Length) const UCHAR *Payload, _In_ ULONG Length);
