/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#pragma once

DEFINE_GUID(GUID_DEVINTERFACE_OEMBRIDGE,
    0xced8991e, 0xad1b, 0x44c7, 0xaf, 0xd8, 0x1d, 0x34, 0x2a, 0x21, 0x58, 0xea);

#define OEMBRIDGE_OUTPUT_MODE_VAR      L"HdmiOutputMode"

DEFINE_GUID(GUID_OEMBRIDGE_DISPLAY_VARS,
    0x798610d0, 0x7000, 0x41e0, 0x80, 0x30, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10);

#define OEMBRIDGE_MODE_AUTO            0
#define OEMBRIDGE_MODE_PACK(w, h, hz)  ((((ULONG)(w) & 0xFFF) << 20) | \
                                        (((ULONG)(h) & 0xFFF) << 8)  | \
                                         ((ULONG)(hz) & 0xFF))
#define OEMBRIDGE_MODE_WIDTH(m)        (((ULONG)(m) >> 20) & 0xFFF)
#define OEMBRIDGE_MODE_HEIGHT(m)       (((ULONG)(m) >> 8) & 0xFFF)
#define OEMBRIDGE_MODE_REFRESH(m)      ((ULONG)(m) & 0xFF)

#define IOCTL_OEMBRIDGE_GET_DISPLAY_INFO \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_OEMBRIDGE_SET_OUTPUT_MODE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_OEMBRIDGE_AUDIO_GET_STATE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_OEMBRIDGE_AUDIO_SET_ENABLE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_OEMBRIDGE_LOGGING_GET_STATE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_OEMBRIDGE_LOGGING_SET_LEVEL \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define OEMBRIDGE_LOG_DISABLED          0
#define OEMBRIDGE_LOG_ERROR             1
#define OEMBRIDGE_LOG_WARNING           2
#define OEMBRIDGE_LOG_INFORMATION       4
#define OEMBRIDGE_LOG_VERBOSE           5

static __inline int OEMBridgeLoggingLevelValid(ULONG Level)
{
    return Level == OEMBRIDGE_LOG_DISABLED || Level == OEMBRIDGE_LOG_ERROR ||
           Level == OEMBRIDGE_LOG_WARNING || Level == OEMBRIDGE_LOG_INFORMATION ||
           Level == OEMBRIDGE_LOG_VERBOSE;
}

#define OEMBRIDGE_MAX_MODES            32

#define OEMBRIDGE_MODE_SRC_PREFERRED   1
#define OEMBRIDGE_MODE_SRC_BASE_DTD    2
#define OEMBRIDGE_MODE_SRC_CEA_VIC     3
#define OEMBRIDGE_MODE_SRC_CEA_DTD     4

#pragma pack(push, 4)

typedef struct _OEMBRIDGE_MODE
{
    USHORT Width;
    USHORT Height;
    USHORT HFront;
    USHORT HSync;
    USHORT HBack;
    USHORT VFront;
    USHORT VSync;
    USHORT VBack;
    UCHAR  HPolPositive;
    UCHAR  VPolPositive;
    UCHAR  Vic;
    UCHAR  Source;
    ULONG  PclkKhz;
    ULONG  RefreshCentiHz;
    ULONG  Packed;
} OEMBRIDGE_MODE;

typedef struct _OEMBRIDGE_DISPLAY_INFO
{
    ULONG          Size;
    ULONG          RequestedMode;

    LONG           VariableStatus;
    ULONG          ActiveWidth;
    ULONG          ActiveHeight;
    ULONG          ActiveMode;

    UCHAR          SinkPresent;
    UCHAR          EdidValid;
    UCHAR          Reserved[2];
    CHAR           SinkName[14];

    ULONG          ModeCount;
    OEMBRIDGE_MODE Modes[OEMBRIDGE_MAX_MODES];
} OEMBRIDGE_DISPLAY_INFO;

typedef struct _OEMBRIDGE_AUDIO_STATE
{
    ULONG Size;
    ULONG Enabled;
} OEMBRIDGE_AUDIO_STATE;

typedef struct _OEMBRIDGE_LOGGING_STATE
{
    ULONG Size;
    ULONG Level;
} OEMBRIDGE_LOGGING_STATE;

#define IOCTL_OEMBRIDGE_CEC_GET_STATE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_OEMBRIDGE_CEC_SET_ENABLE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x811, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_OEMBRIDGE_CEC_SET_LOGICAL_ADDRESS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x812, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_OEMBRIDGE_CEC_TRANSMIT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x813, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_OEMBRIDGE_CEC_RECEIVE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x814, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define OEMBRIDGE_CEC_MAX_FRAME        16

#define OEMBRIDGE_CEC_TX_OK            0
#define OEMBRIDGE_CEC_TX_NACK          1
#define OEMBRIDGE_CEC_TX_LINE          2
#define OEMBRIDGE_CEC_TX_ERROR         3
#define OEMBRIDGE_CEC_TX_TIMEOUT       4

typedef struct _OEMBRIDGE_CEC_FRAME
{
    UCHAR Length;
    UCHAR Data[OEMBRIDGE_CEC_MAX_FRAME];
    UCHAR Reserved[3];
} OEMBRIDGE_CEC_FRAME;

typedef struct _OEMBRIDGE_CEC_STATE
{
    ULONG  Size;
    UCHAR  Enabled;
    UCHAR  HwOn;
    UCHAR  LogicalAddress;
    UCHAR  SinkPresent;
    USHORT PhysicalAddress;
    USHORT Reserved;
    ULONG  TxOk;
    ULONG  TxNack;
    ULONG  TxError;
    ULONG  RxFrames;
    ULONG  RxDropped;
    ULONG  I2cErrors;
} OEMBRIDGE_CEC_STATE;

#pragma pack(pop)
