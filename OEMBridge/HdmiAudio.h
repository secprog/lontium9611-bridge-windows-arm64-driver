/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#pragma once

EXTERN_C_START

#define HDMI_AUDIO_HOTPLUG_IOCTL    0x90012080

#define HDMI_AUDIO_FXN_TABLE_SIZE   0x60

#define HDMI_AUDIO_EDID_BLOCK       128

#define HDMI_AUDIO_MINIPORT_TAG     L"AUCD#VEN_QCOM&DEV_0E29"
#define HDMI_AUDIO_REF_STRING_SLASH L"\\QC_AUDIO_MINIPORT_HDMI_REF_STRING"

typedef struct _HDMI_AUDIO_CONTEXT
{
    WDFDEVICE FxDevice;
    PVOID NotificationEntry;
    PVOID KsNotificationEntry;
    WDFWORKITEM WorkItem;
    WDFTIMER RetryTimer;
    WDFWAITLOCK Lock;
    WDFWAITLOCK CallLock;

    UNICODE_STRING SymbolicLink;
    WCHAR SymbolicLinkBuffer[320];

    WDFIOTARGET Target;
    BOOLEAN Available;
    BOOLEAN Enabled;
    BOOLEAN Established;
    BOOLEAN Active;
    BOOLEAN KsRouteAvailable;
    BOOLEAN ViaKsRoute;
    BOOLEAN WantPresent;
    BOOLEAN ReportedPresent;
    ULONG Retries;
    NTSTATUS LastStatus;

    DECLSPEC_ALIGN(8) UCHAR OursTable[HDMI_AUDIO_FXN_TABLE_SIZE];
    DECLSPEC_ALIGN(8) UCHAR AudioTable[HDMI_AUDIO_FXN_TABLE_SIZE];

    UCHAR Edid[HDMI_AUDIO_EDID_BLOCK];
    BOOLEAN EdidValid;
    GUID ContainerId;

    LONG StreamsRunning;

    ULONG FormatRateHz;
    ULONG FormatBits;
    ULONG FormatChannels;
    WDFWORKITEM FormatWorkItem;
} HDMI_AUDIO_CONTEXT, *PHDMI_AUDIO_CONTEXT;

NTSTATUS HdmiAudioCreate(_In_ WDFDEVICE Device);
VOID     HdmiAudioLoadSettings(_In_ WDFDEVICE Device);
NTSTATUS HdmiAudioPrepare(_In_ WDFDEVICE Device);
VOID     HdmiAudioRelease(_In_ WDFDEVICE Device);
BOOLEAN  HdmiAudioIsEnabled(_In_ WDFDEVICE Device);
BOOLEAN  HdmiAudioHandleIoctl(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ ULONG IoControlCode);

VOID HdmiAudioSetSinkPresent(_In_ WDFDEVICE Device, _In_ BOOLEAN Present);
VOID HdmiAudioCacheEdid(_In_ WDFDEVICE Device, _In_reads_bytes_(Length) const UCHAR *Edid, _In_ ULONG Length);
VOID HdmiAudioGetFormat(_In_ WDFDEVICE Device, _Out_ PULONG RateHz, _Out_ PULONG Bits);

EXTERN_C_END
