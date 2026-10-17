/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#pragma once

#define HDMI_AUDIO_API_MAJOR       1
#define HDMI_AUDIO_API_MINOR       4
#define HDMI_AUDIO_RATE_48000      0x04
#define HDMI_AUDIO_DEPTH_16        0x01
#define HDMI_AUDIO_DEPTH_20        0x02
#define HDMI_AUDIO_DEPTH_24        0x04
#define HDMI_AUDIO_STEREO          0x01
#define HDMI_AUDIO_STREAM_START    0

typedef struct _HDMI_AUDIO_FORMAT
{
    GUID Encoding;
    ULONG Channels;
    ULONG SampleRates;
    BOOL DepthValid;
    ULONG Depths;
    BOOL BitRateValid;
    ULONG BitRate;
    ULONG Speakers;
} HDMI_AUDIO_FORMAT;

typedef struct _HDMI_AUDIO_MODES
{
    ULONG Count;
    HDMI_AUDIO_FORMAT Formats[16];
} HDMI_AUDIO_MODES;

typedef struct _HDMI_AUDIO_PROPERTIES
{
    GUID Encoding;
    ULONG Channels;
    ULONG SampleRate;
    ULONG Depth;
    BOOL InhibitDownmix;
    ULONG LevelShift;
    ULONG Speakers;
} HDMI_AUDIO_PROPERTIES;

typedef struct _HDMI_AUDIO_SINK
{
    KSJACK_SINK_INFORMATION Jack;
    GUID Container;
    ULONG Reserved[16];
} HDMI_AUDIO_SINK;

typedef struct _HDMI_AUDIO_ROUTING
{
    ULONG Controller;
    ULONG Stream;
    ULONG Reserved[4];
} HDMI_AUDIO_ROUTING;

typedef struct _HDMI_AUDIO_PROVIDER_API
{
    ULONG Major;
    ULONG Minor;
    PVOID Context;
    ULONG PortIndex;
    ULONG Reserved[7];
    NTSTATUS (*GetModes)(PVOID Context, HDMI_AUDIO_MODES *Modes);
    NTSTATUS (*SetProperties)(PVOID Context, HDMI_AUDIO_PROPERTIES *Properties);
    NTSTATUS (*GetSink)(PVOID Context, HDMI_AUDIO_SINK *Sink);
    NTSTATUS (*SetHdcp)(PVOID Context, BOOL Enabled);
    NTSTATUS (*GetRouting)(PVOID Context, HDMI_AUDIO_ROUTING *Routing);
    NTSTATUS (*ChangeState)(PVOID Context, ULONG State);
} HDMI_AUDIO_PROVIDER_API;

typedef struct _HDMI_AUDIO_CONSUMER_API
{
    ULONG Major;
    ULONG Minor;
    PVOID Context;
    ULONG Reserved[8];
    NTSTATUS (*Hotplug)(PVOID Context, BOOL Connected);
    NTSTATUS (*Unloaded)(PVOID Context);
    NTSTATUS (*ModeChanged)(PVOID Context);
    PVOID ReservedCallbacks[3];
} HDMI_AUDIO_CONSUMER_API;

C_ASSERT(sizeof(HDMI_AUDIO_FORMAT) == 44);
C_ASSERT(sizeof(HDMI_AUDIO_MODES) == 708);
C_ASSERT(sizeof(HDMI_AUDIO_PROPERTIES) == 40);
C_ASSERT(sizeof(HDMI_AUDIO_ROUTING) == 24);
C_ASSERT(sizeof(HDMI_AUDIO_PROVIDER_API) == 96);
C_ASSERT(sizeof(HDMI_AUDIO_CONSUMER_API) == 96);
C_ASSERT(FIELD_OFFSET(HDMI_AUDIO_PROVIDER_API, PortIndex) == 16);
C_ASSERT(FIELD_OFFSET(HDMI_AUDIO_PROVIDER_API, GetModes) == 48);
C_ASSERT(FIELD_OFFSET(HDMI_AUDIO_PROVIDER_API, ChangeState) == 88);
C_ASSERT(FIELD_OFFSET(HDMI_AUDIO_CONSUMER_API, Hotplug) == 48);
