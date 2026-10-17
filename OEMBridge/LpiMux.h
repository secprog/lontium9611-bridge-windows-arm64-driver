/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#pragma once

EXTERN_C_START

#define ADCM_AUDIOHWRSC_INTERFACE_VERSION   1

#define ADCM_AUDIOHWRSC_CMD_ENABLE_CLOCK    5
#define ADCM_AUDIOHWRSC_CMD_DISABLE_CLOCK   6

#define PARAM_ID_RSC_AUDIO_HW_CLK           0x0800102C
#define LPASS_CLOCK_ID_MCLK_1               0x00000300
#define LPASS_CLOCK_MCLK_1_HZ               12288000
#define LPASS_CLOCK_ATTRIBUTE_COUPLE_NO     1
#define LPASS_CLOCK_ROOT_DEFAULT            0

#define LPASS_LPI_TLMM_PA                   0x033C0000ULL
#define LPASS_LPI_TLMM_LENGTH               0x00002008
#define LPASS_LPI_PIN_STRIDE                0x1000
#define LPASS_LPI_QUA_PIN_COUNT             3
#define LPASS_LPI_QUA_CFG                   0x2C8
#define LPASS_LPI_QUA_VAL                   0x002

#pragma pack(push, 4)

typedef struct _ADCM_HW_CLK_REQUEST
{
    ULONG ModuleInstanceId;
    ULONG ParamId;
    ULONG ParamSize;
    ULONG ErrorCode;
    ULONG NumClockId;
    ULONG ClockId;
    ULONG ClockFreqHz;
    ULONG ClockAttribute;
    ULONG ClockRoot;
} ADCM_HW_CLK_REQUEST, *PADCM_HW_CLK_REQUEST;

typedef struct _ADCM_HW_CLK_RELEASE
{
    ULONG ModuleInstanceId;
    ULONG ParamId;
    ULONG ParamSize;
    ULONG ErrorCode;
    ULONG NumClockId;
    ULONG ClockId;
} ADCM_HW_CLK_RELEASE, *PADCM_HW_CLK_RELEASE;

C_ASSERT(sizeof(ADCM_HW_CLK_REQUEST) == 0x24);
C_ASSERT(sizeof(ADCM_HW_CLK_RELEASE) == 0x18);

#pragma pack(pop)

typedef struct _ADCM_HW_RSC_DESCRIPTOR
{
    PVOID Payload;
    ULONG PayloadSize;
    ULONG Reserved;
} ADCM_HW_RSC_DESCRIPTOR, *PADCM_HW_RSC_DESCRIPTOR;

typedef
NTSTATUS
(*PADCM_AUDIOHWRSC_IOCTL)(
    _In_ ULONG Command,
    _In_ PADCM_HW_RSC_DESCRIPTOR Request
    );

typedef struct _ADCM_AUDIOHWRSC_INTERFACE
{
    INTERFACE InterfaceHeader;
    PADCM_AUDIOHWRSC_IOCTL AudioHwRscIoctl;
} ADCM_AUDIOHWRSC_INTERFACE, *PADCM_AUDIOHWRSC_INTERFACE;

C_ASSERT(sizeof(ADCM_AUDIOHWRSC_INTERFACE) == 0x28);

typedef struct _LPI_MUX_CONTEXT
{
    WDFDEVICE FxDevice;
    PVOID NotificationEntry;
    WDFWORKITEM WorkItem;
    WDFTIMER RetryTimer;
    WDFWAITLOCK Lock;

    UNICODE_STRING SymbolicLink;
    WCHAR SymbolicLinkBuffer[256];

    WDFIOTARGET Target;
    ADCM_AUDIOHWRSC_INTERFACE Interface;
    BOOLEAN InterfaceAcquired;
    BOOLEAN TargetGone;
    BOOLEAN ClockEnabled;
    BOOLEAN MuxApplied;
    ULONG Retries;
    NTSTATUS LastStatus;
    ULONG LastDspError;

    volatile UCHAR *TlmmBase;
    ULONG TlmmLength;
} LPI_MUX_CONTEXT, *PLPI_MUX_CONTEXT;

NTSTATUS
LpiMuxCreate(
    _In_ WDFDEVICE Device
    );

NTSTATUS
LpiMuxPrepare(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    );

VOID
LpiMuxRelease(
    _In_ WDFDEVICE Device
    );

EXTERN_C_END
