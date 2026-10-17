/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#include "driver.h"
#include "gpio.h"
#include "Qdi.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, OEMBridgeCreateDevice)
#endif

#define BIT(x) (1 << (x))

#define CFG_HPD_INTERRUPTS BIT(0)

NTSTATUS LT9611Write(WDFDEVICE FxDevice, UCHAR Address, UCHAR Data)
{
	NTSTATUS status;
	PDEVICE_CONTEXT devContext = DeviceGetContext(FxDevice);
	UCHAR buffer[1] = { Data };

	status = SpbWriteDataSynchronously(&devContext->I2CContext, Address, buffer, 1);
	return status;
}

NTSTATUS LT9611Read(WDFDEVICE FxDevice, UCHAR Address, UCHAR* Data, ULONG Length)
{
	NTSTATUS status;
	PDEVICE_CONTEXT devContext = DeviceGetContext(FxDevice);
	status = SpbReadDataSynchronously(&devContext->I2CContext, Address, Data, Length);
	return status;
}

struct LT9611RegisterCfg {
	UCHAR reg;
	UCHAR val;
	int sleep_in_ms;
};

static VOID RubikDelayMs(int ms)
{
    LARGE_INTEGER d;
    d.QuadPart = -10000LL * ms;
    KeDelayExecutionThread(KernelMode, FALSE, &d);
}

NTSTATUS LT9611WriteArray(WDFDEVICE FxDevice, struct LT9611RegisterCfg* cfg, size_t size)
{
	NTSTATUS status;

	size = size / sizeof(struct LT9611RegisterCfg);

	for (size_t i = 0; i < size; i++)
	{
		status = LT9611Write(FxDevice, cfg[i].reg, cfg[i].val);

		if (!NT_SUCCESS(status)) {
			Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Failed to write reg %d val %d", cfg[i].reg, cfg[i].val);
			return status;
		}

		if (cfg[i].sleep_in_ms) {
			RubikDelayMs(cfg[i].sleep_in_ms);
		}
	}
	return STATUS_SUCCESS;
}

NTSTATUS LT9611HDMIOutputEnable(WDFDEVICE FxDevice);
NTSTATUS LT9611HDMIOutputDisable(WDFDEVICE FxDevice);
NTSTATUS LT9611AudioEnable(WDFDEVICE FxDevice);
NTSTATUS LT9611VideoRelock(WDFDEVICE FxDevice);
NTSTATUS LT9611VideoRelockEx(WDFDEVICE FxDevice, int *SelectedMode);
NTSTATUS LT9611VideoOn(WDFDEVICE FxDevice, BOOLEAN PowerOn);
NTSTATUS LT9611Write(WDFDEVICE FxDevice, UCHAR Address, UCHAR Data);

EVT_WDF_TIMER OnHealthTimer;

NTSTATUS
LT9611Reset(WDFDEVICE FxDevice, BOOLEAN PowerCycle)
{
    unsigned char value;

	PDEVICE_CONTEXT devContext = DeviceGetContext(FxDevice);
    if (PowerCycle)
    {

        value = 1;
        SetGPIO(devContext->ResetGpio, &value);
        RubikDelayMs(20);

        value = 0;
        SetGPIO(devContext->ResetGpio, &value);
        RubikDelayMs(20);

        value = 1;
        SetGPIO(devContext->ResetGpio, &value);
        RubikDelayMs(100);
    }
    else
    {
        value = 0;
        SetGPIO(devContext->ResetGpio, &value);
    }

	return STATUS_SUCCESS;
}

NTSTATUS LT9611EnableInterrupts(WDFDEVICE FxDevice, int interrupts)
{
	NTSTATUS status;
	UCHAR reg_val = 0;
	UCHAR init_reg_val;

	if (interrupts & CFG_HPD_INTERRUPTS) {
		LT9611Write(FxDevice, 0xff, 0x82);
		LT9611Read(FxDevice, 0x03, &reg_val, 1);

		if (reg_val & 0xc0)
        {
			init_reg_val = reg_val & 0x3f;
			Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "enabling hpd interrupts");
			LT9611Write(FxDevice, 0x03, init_reg_val);
		}

		LT9611Write(FxDevice, 0x07, 0xff);
		LT9611Write(FxDevice, 0x07, 0x3f);
	}

    status = STATUS_SUCCESS;

	return status;
}

NTSTATUS LT9611PowerOn(WDFDEVICE FxDevice, BOOLEAN PowerOn)
{
    NTSTATUS status;
    PDEVICE_CONTEXT devContext = DeviceGetContext(FxDevice);

    if (PowerOn && !devContext->PowerOn)
    {

        status = LT9611EnableInterrupts(FxDevice, CFG_HPD_INTERRUPTS);

        if (!NT_SUCCESS(status))
        {
			Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Failed to enable HPD intr %d", status);
            return status;
        }

        devContext->PowerOn = TRUE;
    }
    else if (!PowerOn)
    {
        LT9611Write(FxDevice, 0xff, 0x81);
        LT9611Write(FxDevice, 0x30, 0x6a);

        devContext->PowerOn = FALSE;
    }

    status = STATUS_SUCCESS;

    return status;
}

NTSTATUS LT9611HDMIOutputEnable(WDFDEVICE FxDevice)
{
	NTSTATUS status;

	status = LT9611Write(FxDevice, 0xff, 0x81);
	if (!NT_SUCCESS(status))
	{
		Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Failed to write reg %d val %d", 0xff, 0x81);
		return status;
	}

    status = LT9611Write(FxDevice, 0x30, 0xea);
    if (!NT_SUCCESS(status))
    {
		Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Failed to write reg %d val %d", 0x30, 0xea);
		return status;
    }

	return status;
}

NTSTATUS LT9611HDMIOutputDisable(WDFDEVICE FxDevice)
{
	NTSTATUS status;

	status = LT9611Write(FxDevice, 0xff, 0x81);
	if (!NT_SUCCESS(status))
	{
		Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Failed to write reg %d val %d", 0xff, 0x81);
		return status;
	}

	status = LT9611Write(FxDevice, 0x30, 0x6a);
	if (!NT_SUCCESS(status))
	{
		Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Failed to write reg %d val %d", 0x30, 0x6a);
		return status;
	}

	return status;
}

#define LT9611_INFOFRAME_ENABLE_REG 0x3d
#define LT9611_INFOFRAME_AUDIO      0x02

static UCHAR LT9611AudioSclkReg(ULONG Bits)
{
    return (Bits > 16) ? 0xd4 : 0xd5;
}

NTSTATUS LT9611AudioProgram(WDFDEVICE FxDevice, ULONG RateHz, ULONG Bits)
{
    NTSTATUS status;
    UCHAR enable_val = 0;
    UCHAR fs_reg, n2, n1, n0;
    size_t i;

    static const UCHAR AudioInfoFrame[] = {
        0x84, 0x01, 0x0A, 0x70,
        0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00,
    };

    switch (RateHz)
    {
    case 44100: fs_reg = 0x0b; n2 = 0x00; n1 = 0x18; n0 = 0x80; break;
    case 96000: fs_reg = 0xab; n2 = 0x00; n1 = 0x30; n0 = 0x00; break;
    default:    fs_reg = 0x2b; n2 = 0x00; n1 = 0x18; n0 = 0x00; RateHz = 48000; break;
    }

    {
        struct LT9611RegisterCfg audio[] = {
            {0xff, 0x82, 0}, {0xd6, 0x8e, 0}, {0xd7, 0x04, 0},
            {0xff, 0x84, 0}, {0x06, 0x08, 0}, {0x07, 0x10, 0},
            {0x34, LT9611AudioSclkReg(Bits), 0},
            {0x0f, fs_reg, 0}, {0x35, n2, 0}, {0x36, n1, 0}, {0x37, n0, 0},
        };
        status = LT9611WriteArray(FxDevice, audio, sizeof(audio));
        if (!NT_SUCCESS(status)) return status;
    }

    for (i = 0; i < sizeof(AudioInfoFrame); i++)
    {
        status = LT9611Write(FxDevice, (UCHAR)(0xb2 + i), AudioInfoFrame[i]);
        if (!NT_SUCCESS(status)) return status;
    }

    status = LT9611Read(FxDevice, LT9611_INFOFRAME_ENABLE_REG, &enable_val, 1);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    status = LT9611Write(FxDevice, LT9611_INFOFRAME_ENABLE_REG,
                         enable_val | LT9611_INFOFRAME_AUDIO);

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
          "LT9611 HDMI audio enabled (I2S %lu Hz %lu-bit 2ch, sclk %s, 84:34=0x%02x, InfoFrame en=0x%x)",
          RateHz, Bits, (Bits > 16) ? "64fs" : "32fs", LT9611AudioSclkReg(Bits),
          enable_val | LT9611_INFOFRAME_AUDIO);

    return status;
}

NTSTATUS LT9611AudioDisable(WDFDEVICE FxDevice)
{
    NTSTATUS status;
    UCHAR enable_val;

    status = LT9611Write(FxDevice, 0xff, 0x84);
    if (!NT_SUCCESS(status)) return status;
    status = LT9611Read(FxDevice, LT9611_INFOFRAME_ENABLE_REG, &enable_val, 1);
    if (!NT_SUCCESS(status)) return status;
    status = LT9611Write(FxDevice, LT9611_INFOFRAME_ENABLE_REG,
                         (UCHAR)(enable_val & ~LT9611_INFOFRAME_AUDIO));
    if (!NT_SUCCESS(status)) return status;
    status = LT9611Write(FxDevice, 0x06, 0x00);
    if (!NT_SUCCESS(status)) return status;
    return LT9611Write(FxDevice, 0x07, 0x00);
}

NTSTATUS LT9611AudioEnable(WDFDEVICE FxDevice)
{
    ULONG rate = 48000, bits = 16;
    if (!HdmiAudioIsEnabled(FxDevice))
    {
        return LT9611AudioDisable(FxDevice);
    }
    HdmiAudioGetFormat(FxDevice, &rate, &bits);
    return LT9611AudioProgram(FxDevice, rate, bits);
}

static struct LT9611RegisterCfg RubikInitSetup[] = {
    {0xff,0x81,0},{0x01,0x18,0},
    {0xff,0x82,0},{0x51,0x11,0},
    {0x1b,0x69,0},{0x1c,0x78,0},{0xcb,0x69,0},{0xcc,0x78,0},

    {0x58,0x0a,0},{0x59,0x80,0},{0x9e,0xf7,0},
    {0xff,0x80,0},{0x04,0xf0,0},{0x06,0xf0,0},{0x0a,0x80,0},{0x0b,0x46,0},
    {0x0d,0xef,0},{0x11,0xfa,0},
};
static struct LT9611RegisterCfg RubikMipiInputAnalog[] = {
    {0xff,0x81,0},{0x06,0x60,0},{0x07,0x3f,0},{0x08,0x3f,0},{0x0a,0xfe,0},{0x0b,0xbf,0},
    {0x11,0x60,0},{0x12,0x3f,0},{0x13,0x3f,0},{0x15,0xfe,0},{0x16,0xbf,0},
    {0x1c,0x03,0},{0x20,0x03,0},
};
static struct LT9611RegisterCfg RubikMipiInputDigital[] = {
    {0xff,0x82,0},{0x50,0x14,0},
    {0xff,0x83,0},{0x03,0x40,0},
    {0xff,0x82,0},{0x4f,0x80,0},
    {0xff,0x83,0},{0x00,0x00,0},{0x02,0x0a,0},{0x06,0x0a,0},{0x0a,0x00,0},
};
static struct LT9611RegisterCfg RubikHdmiTxPhy[] = {
    {0xff,0x81,0},{0x30,0x6a,0},{0x31,0x44,0},{0x32,0x4a,0},{0x33,0x0b,0},
    {0x34,0x00,0},{0x35,0x00,0},{0x36,0x00,0},{0x37,0x44,0},
    {0x3f,0x0f,0},{0x40,0x98,0},{0x41,0x98,0},{0x42,0x98,0},{0x43,0x98,0},{0x44,0x0a,0},
};

typedef struct {
    UINT32 hFront, hPulse, hBack, hActive;
    UINT32 vFront, vPulse, vBack, vActive;
    UINT32 hPol, vPol, vic, pclkKhz;
} RUBIK_VIDEO_CFG;

static const RUBIK_VIDEO_CFG RubikVideoTab[] = {
    {   8,  96,  40,  640,  33,  2, 10,  480, 0, 0,  0,  25000 },
    {  16,  62,  60,  720,   9,  6, 30,  480, 0, 0,  0,  27000 },
    {  60,  60, 100, 1024,   2,  5, 10,  600, 0, 0,  0,  34000 },
    { 110,  40, 220, 1280,   5,  5, 20,  720, 1, 1,  0,  74250 },
    {  88,  44, 148, 1920,   4,  5, 36, 1080, 1, 1,  0,  74250 },
    {  88,  44, 148, 1920,   4,  5, 36, 1080, 1, 1, 16, 148500 },
    {  48,  32,  80, 2560,   3,  5, 33, 1440, 1, 1,  0, 241500 },
    { 176,  88, 296, 3840,   4,  5, 36, 1080, 1, 1,  0, 297000 },
    { 176,  88, 296, 3840,   8, 10, 72, 2160, 1, 1, 95, 297000 },
};
#define RUBIK_MODE_1024x600   2
#define RUBIK_MODE_DEFAULT    5
#define RUBIK_MODE_4K30       8
#define RUBIK_MODE_COUNT      (sizeof(RubikVideoTab) / sizeof(RubikVideoTab[0]))

#define RUBIK_MODE_EDID_BASE  100

static UCHAR RubikPllSetup(WDFDEVICE FxDevice, const RUBIK_VIDEO_CFG *cfg)
{
    static struct LT9611RegisterCfg txpll[] = {
        {0xff,0x81,0},{0x23,0x40,0},{0x24,0x64,0},{0x25,0x80,0},{0x26,0x55,0},
        {0x2c,0x37,0},{0x2f,0x01,0},{0x27,0x66,0},{0x28,0x88,0},{0x2a,0x20,0},
    };
    UINT32 pclk = cfg->pclkKhz;
    UINT32 postDiv;
    UCHAR pcrM, lock = 0, band = 0, cal = 0;
    int i;

    LT9611WriteArray(FxDevice, txpll, sizeof(txpll));
    LT9611Write(FxDevice, 0xff, 0x81);
    if (pclk > 150000)      { LT9611Write(FxDevice, 0x2d, 0x88); postDiv = 1; }
    else if (pclk > 70000)  { LT9611Write(FxDevice, 0x2d, 0x99); postDiv = 2; }
    else                    { LT9611Write(FxDevice, 0x2d, 0xaa); postDiv = 4; }

    pcrM = (UCHAR)((pclk * 5 * postDiv) / 27000);

    LT9611Write(FxDevice, 0xff, 0x83);
    LT9611Write(FxDevice, 0x2d, 0x38);
    LT9611Write(FxDevice, 0x31, 0x08);
    LT9611Write(FxDevice, 0x26, pcrM);

    LT9611Write(FxDevice, 0xff, 0x82);
    pclk /= 2;
    LT9611Write(FxDevice, 0xe3, (UCHAR)(pclk / 65536));
    pclk %= 65536;
    LT9611Write(FxDevice, 0xe4, (UCHAR)(pclk / 256));
    LT9611Write(FxDevice, 0xe5, (UCHAR)(pclk % 256));
    LT9611Write(FxDevice, 0xde, 0x20);
    LT9611Write(FxDevice, 0xde, 0xe0);

    LT9611Write(FxDevice, 0xff, 0x80);
    LT9611Write(FxDevice, 0x11, 0x5a);
    LT9611Write(FxDevice, 0x11, 0xfa);
    LT9611Write(FxDevice, 0x16, 0xf2);
    LT9611Write(FxDevice, 0x18, 0xdc);
    LT9611Write(FxDevice, 0x18, 0xfc);
    LT9611Write(FxDevice, 0x16, 0xf3);

    for (i = 0; i < 5; i++)
    {
        LT9611Write(FxDevice, 0x16, 0xe3);
        LT9611Write(FxDevice, 0x16, 0xf3);
        LT9611Write(FxDevice, 0xff, 0x82);
        LT9611Read(FxDevice, 0x15, &lock, 1);
        LT9611Read(FxDevice, 0xe6, &band, 1);
        LT9611Read(FxDevice, 0xe7, &cal, 1);
        if ((lock & 0x80) && (cal & 0x80) && (band != 0xff))
        {
            Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "LT9611 PLL locked (try %d)", i);
            break;
        }
        LT9611Write(FxDevice, 0xff, 0x80);
        LT9611Write(FxDevice, 0x11, 0x5a); RubikDelayMs(10);
        LT9611Write(FxDevice, 0x11, 0xfa);
        LT9611Write(FxDevice, 0x16, 0xf2);
        LT9611Write(FxDevice, 0x18, 0xdc); RubikDelayMs(10);
        LT9611Write(FxDevice, 0x18, 0xfc);
        LT9611Write(FxDevice, 0x16, 0xf3); RubikDelayMs(10);
    }
    if (i == 5)
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "LT9611 PLL did NOT lock");

    return pcrM;
}

static VOID RubikPcrSetup(WDFDEVICE FxDevice, const RUBIK_VIDEO_CFG *cfg)
{
    UCHAR pol = (UCHAR)((cfg->hPol * 0x02) + cfg->vPol);
    pol = (UCHAR)(~pol) & 0x03;

    LT9611Write(FxDevice, 0xff, 0x83);
    LT9611Write(FxDevice, 0x0b, 0x01); LT9611Write(FxDevice, 0x0c, 0x10);
    LT9611Write(FxDevice, 0x48, 0x00); LT9611Write(FxDevice, 0x49, 0x81);
    LT9611Write(FxDevice, 0x21, 0x4a); LT9611Write(FxDevice, 0x24, 0x71);
    LT9611Write(FxDevice, 0x25, 0x30); LT9611Write(FxDevice, 0x2a, 0x01);
    LT9611Write(FxDevice, 0x4a, 0x40); LT9611Write(FxDevice, 0x1d, (UCHAR)(0x10 | pol));

    if (cfg->hActive == 1024 && cfg->vActive == 600)
    {
        LT9611Write(FxDevice, 0x24, 0x70); LT9611Write(FxDevice, 0x25, 0x80);
        LT9611Write(FxDevice, 0x2a, 0x10); LT9611Write(FxDevice, 0x23, 0x04);
        LT9611Write(FxDevice, 0x4a, 0x10); LT9611Write(FxDevice, 0x1d, 0xf0);
    }
}

static VOID RubikMipiVideoSetup(WDFDEVICE FxDevice, const RUBIK_VIDEO_CFG *c)
{
    UINT32 hTot = c->hActive + c->hFront + c->hPulse + c->hBack;
    UINT32 vTot = c->vActive + c->vFront + c->vPulse + c->vBack;
    UINT32 hss  = c->hPulse + c->hBack;
    UINT32 vss  = c->vPulse + c->vBack;

    LT9611Write(FxDevice, 0xff, 0x83);
    LT9611Write(FxDevice, 0x0d, (UCHAR)(vTot / 256));       LT9611Write(FxDevice, 0x0e, (UCHAR)(vTot % 256));
    LT9611Write(FxDevice, 0x0f, (UCHAR)(c->vActive / 256)); LT9611Write(FxDevice, 0x10, (UCHAR)(c->vActive % 256));
    LT9611Write(FxDevice, 0x11, (UCHAR)(hTot / 256));       LT9611Write(FxDevice, 0x12, (UCHAR)(hTot % 256));
    LT9611Write(FxDevice, 0x13, (UCHAR)(c->hActive / 256)); LT9611Write(FxDevice, 0x14, (UCHAR)(c->hActive % 256));
    LT9611Write(FxDevice, 0x15, (UCHAR)(c->vPulse % 256));  LT9611Write(FxDevice, 0x16, (UCHAR)(c->hPulse % 256));
    LT9611Write(FxDevice, 0x17, (UCHAR)(c->vFront % 256));  LT9611Write(FxDevice, 0x18, (UCHAR)(vss % 256));
    LT9611Write(FxDevice, 0x19, (UCHAR)(c->hFront % 256));
    LT9611Write(FxDevice, 0x1a, (UCHAR)(((c->hFront / 256) << 4) + (hss / 256)));
    LT9611Write(FxDevice, 0x1b, (UCHAR)(hss % 256));
}

static VOID RubikTxDigital(WDFDEVICE FxDevice, const RUBIK_VIDEO_CFG *c)
{
    UINT32 vic = c->vic;
    UCHAR  ar  = 0x02;
    UCHAR  pb2 = (UCHAR)((ar << 4) + 0x08);
    UCHAR  pb4 = (vic == 95) ? 0x00 : (UCHAR)vic;
    UCHAR  pb0 = (UCHAR)(((pb2 + pb4) <= 0x5f) ? (0x5f - pb2 - pb4) : (0x15f - pb2 - pb4));
    UCHAR  ifEn = (0x02 | 0x08);

    LT9611Write(FxDevice, 0xff, 0x82);
    LT9611Write(FxDevice, 0xd6, 0x8e); LT9611Write(FxDevice, 0xd7, 0x04);

    LT9611Write(FxDevice, 0xff, 0x84);
    LT9611Write(FxDevice, 0x43, pb0);
    LT9611Write(FxDevice, 0x45, pb2);
    LT9611Write(FxDevice, 0x47, pb4);
    LT9611Write(FxDevice, 0x10, 0x02);
    LT9611Write(FxDevice, 0x12, 0x64);

    if (vic == 95)
    {
        LT9611Write(FxDevice, 0x3d, (UCHAR)(ifEn | 0x20));
        LT9611Write(FxDevice, 0x74, 0x81); LT9611Write(FxDevice, 0x75, 0x01);
        LT9611Write(FxDevice, 0x76, 0x05); LT9611Write(FxDevice, 0x77, 0x49);
        LT9611Write(FxDevice, 0x78, 0x03); LT9611Write(FxDevice, 0x79, 0x0c);
        LT9611Write(FxDevice, 0x7a, 0x00); LT9611Write(FxDevice, 0x7b, 0x20);
        LT9611Write(FxDevice, 0x7c, 0x01);
    }
    else
    {
        LT9611Write(FxDevice, 0x3d, ifEn);
    }
}

static VOID RubikMeasureInput(WDFDEVICE FxDevice, UINT32 *hAct, UINT32 *vAct, UINT32 *hTotSysclk)
{
    UCHAR hi = 0, lo = 0;

    LT9611Write(FxDevice, 0xff, 0x82);
    LT9611Read(FxDevice, 0x82, &hi, 1); LT9611Read(FxDevice, 0x83, &lo, 1);
    *vAct = ((UINT32)hi << 8) | lo;
    LT9611Read(FxDevice, 0x86, &hi, 1); LT9611Read(FxDevice, 0x87, &lo, 1);
    *hTotSysclk = ((UINT32)hi << 8) | lo;

    LT9611Write(FxDevice, 0xff, 0x83);
    LT9611Read(FxDevice, 0x82, &hi, 1); LT9611Read(FxDevice, 0x83, &lo, 1);
    *hAct = (((UINT32)hi << 8) | lo) / 3;
}

static VOID RubikCfgFromSinkMode(const OEMBRIDGE_MODE *m, RUBIK_VIDEO_CFG *c)
{
    c->hFront  = m->HFront;  c->hPulse = m->HSync;  c->hBack = m->HBack;  c->hActive = m->Width;
    c->vFront  = m->VFront;  c->vPulse = m->VSync;  c->vBack = m->VBack;  c->vActive = m->Height;
    c->hPol    = m->HPolPositive;
    c->vPol    = m->VPolPositive;
    c->vic     = m->Vic;
    c->pclkKhz = m->PclkKhz;
}

static UINT32 RubikExpectedHts(const RUBIK_VIDEO_CFG *c)
{
    UINT32 hTot = c->hActive + c->hFront + c->hPulse + c->hBack;
    return (c->pclkKhz != 0) ? (hTot * 27000) / c->pclkKhz : 0;
}

static int RubikSelectMode(PDEVICE_CONTEXT ctx, UINT32 hAct, UINT32 vAct, UINT32 hTotSysclk)
{
    int best = -1;
    UINT32 bestErr = (UINT32)-1;
    UINT32 i;

    if (hAct == 0 || vAct == 0)
    {
        return -1;
    }

    for (i = 0; i < RUBIK_MODE_COUNT; i++)
    {
        const RUBIK_VIDEO_CFG *c = &RubikVideoTab[i];
        UINT32 expected, err;

        if (c->hActive != hAct || c->vActive != vAct)
        {
            continue;
        }
        expected = RubikExpectedHts(c);
        err = (hTotSysclk > expected) ? (hTotSysclk - expected)
                                      : (expected - hTotSysclk);
        if (err < bestErr)
        {
            bestErr = err;
            best = (int)i;
        }
    }

    for (i = 0; i < ctx->SinkModeCount; i++)
    {
        RUBIK_VIDEO_CFG c;
        UINT32 expected, err;

        RubikCfgFromSinkMode(&ctx->SinkModes[i], &c);
        if (c.hActive != hAct || c.vActive != vAct)
        {
            continue;
        }
        expected = RubikExpectedHts(&c);
        err = (hTotSysclk > expected) ? (hTotSysclk - expected)
                                      : (expected - hTotSysclk);
        if (err < bestErr)
        {
            bestErr = err;
            best = RUBIK_MODE_EDID_BASE + (int)i;
        }
    }
    return best;
}

static const RUBIK_VIDEO_CFG *RubikModeCfg(PDEVICE_CONTEXT ctx, int mode, RUBIK_VIDEO_CFG *Scratch)
{
    if (mode >= RUBIK_MODE_EDID_BASE)
    {
        ULONG i = (ULONG)(mode - RUBIK_MODE_EDID_BASE);

        if (i < ctx->SinkModeCount)
        {
            RubikCfgFromSinkMode(&ctx->SinkModes[i], Scratch);
            return Scratch;
        }
        return &RubikVideoTab[RUBIK_MODE_DEFAULT];
    }
    return &RubikVideoTab[(mode < 0 || mode >= (int)RUBIK_MODE_COUNT) ? RUBIK_MODE_DEFAULT : mode];
}

ULONG LT9611LockedModePacked(PDEVICE_CONTEXT ctx)
{
    RUBIK_VIDEO_CFG        scratch;
    const RUBIK_VIDEO_CFG *c;
    ULONGLONG              total;
    ULONG                  hz;

    if (ctx->LockedMode < 0)
    {
        return 0;
    }
    if (ctx->LockedMode >= RUBIK_MODE_EDID_BASE)
    {
        ULONG i = (ULONG)(ctx->LockedMode - RUBIK_MODE_EDID_BASE);
        return (i < ctx->SinkModeCount) ? ctx->SinkModes[i].Packed : 0;
    }
    c = RubikModeCfg(ctx, ctx->LockedMode, &scratch);
    total = (ULONGLONG)(c->hActive + c->hFront + c->hPulse + c->hBack) *
            (ULONGLONG)(c->vActive + c->vFront + c->vPulse + c->vBack);
    if (total == 0)
    {
        return 0;
    }
    hz = (ULONG)(((ULONGLONG)c->pclkKhz * 1000ULL + total / 2) / total);
    return OEMBRIDGE_MODE_PACK(c->hActive, c->vActive, hz);
}

static UINT32 LT9611InputVActive(WDFDEVICE FxDevice)
{
    UCHAR hi = 0, lo = 0;
    LT9611Write(FxDevice, 0xff, 0x82);
    LT9611Read(FxDevice, 0x82, &hi, 1);
    LT9611Read(FxDevice, 0x83, &lo, 1);
    return ((UINT32)hi << 8) | lo;
}

NTSTATUS LT9611VideoRelockEx(WDFDEVICE FxDevice, int *SelectedMode)
{
    PDEVICE_CONTEXT devContext = DeviceGetContext(FxDevice);
    UINT32 hAct = 0, vAct = 0, hTotSysclk = 0;
    const RUBIK_VIDEO_CFG *cfg;
    RUBIK_VIDEO_CFG scratch;
    UCHAR pcrM;
    int mode;

    RubikMeasureInput(FxDevice, &hAct, &vAct, &hTotSysclk);
    mode = RubikSelectMode(devContext, hAct, vAct, hTotSysclk);
    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
          "relock: input %dx%d hts=%d -> mode %d",
          hAct, vAct, hTotSysclk, mode);
    if (SelectedMode != NULL)
    {
        *SelectedMode = mode;
    }
    cfg = RubikModeCfg(devContext, mode, &scratch);

    LT9611Write(FxDevice, 0xff, 0x81);
    LT9611Write(FxDevice, 0x30, 0x00);
    LT9611Write(FxDevice, 0x23, 0x80);

    LT9611Write(FxDevice, 0xff, 0x83);
    if (cfg->hActive == 3840 && cfg->vActive == 2160)
    {
        LT9611Write(FxDevice, 0x02, 0x10);
        LT9611Write(FxDevice, 0x06, 0x10);
    }
    else
    {
        LT9611Write(FxDevice, 0x02, 0x0a);
        LT9611Write(FxDevice, 0x06, 0x0a);
    }

    pcrM = RubikPllSetup(FxDevice, cfg);
    RubikPcrSetup(FxDevice, cfg);
    RubikMipiVideoSetup(FxDevice, cfg);

    LT9611Write(FxDevice, 0xff, 0x83);
    LT9611Write(FxDevice, 0x26, pcrM);

    LT9611Write(FxDevice, 0xff, 0x80);
    LT9611Write(FxDevice, 0x11, 0x5a);
    RubikDelayMs(10);
    LT9611Write(FxDevice, 0x11, 0xfa);

    RubikTxDigital(FxDevice, cfg);

    LT9611Write(FxDevice, 0xff, 0x81);
    LT9611Write(FxDevice, 0x23, 0x40);
    LT9611Write(FxDevice, 0xff, 0x82);
    LT9611Write(FxDevice, 0xde, 0x20);
    LT9611Write(FxDevice, 0xde, 0xe0);
    LT9611Write(FxDevice, 0xff, 0x80);
    LT9611Write(FxDevice, 0x18, 0xdc);
    LT9611Write(FxDevice, 0x18, 0xfc);
    LT9611Write(FxDevice, 0x16, 0xf1);
    LT9611Write(FxDevice, 0x16, 0xf3);
    LT9611Write(FxDevice, 0x11, 0x5a);
    LT9611Write(FxDevice, 0x11, 0xfa);

    if (mode >= 0)
    {
        LT9611Write(FxDevice, 0xff, 0x81);
        LT9611Write(FxDevice, 0x30, 0xea);
        LT9611AudioEnable(FxDevice);
    }
    else
    {
        Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
              "relock: input dead - staying muted");
    }

    return STATUS_SUCCESS;
}

NTSTATUS LT9611VideoRelock(WDFDEVICE FxDevice)
{
    return LT9611VideoRelockEx(FxDevice, NULL);
}

NTSTATUS LT9611VideoOn(WDFDEVICE FxDevice, BOOLEAN PowerOn)
{
    NTSTATUS status;

    if (PowerOn)
    {
        int tries;

        LT9611WriteArray(FxDevice, RubikInitSetup, sizeof(RubikInitSetup));
        LT9611WriteArray(FxDevice, RubikMipiInputAnalog, sizeof(RubikMipiInputAnalog));
        LT9611WriteArray(FxDevice, RubikMipiInputDigital, sizeof(RubikMipiInputDigital));
        LT9611WriteArray(FxDevice, RubikHdmiTxPhy, sizeof(RubikHdmiTxPhy));

        LT9611Write(FxDevice, 0xff, 0x81);
        LT9611Write(FxDevice, 0x02, 0x12);
        LT9611Write(FxDevice, 0x23, 0x40);
        LT9611Write(FxDevice, 0xff, 0x80);
        LT9611Write(FxDevice, 0x11, 0xfa);
        RubikDelayMs(100);

        LT9611VideoRelock(FxDevice);

        for (tries = 0; tries < 5; tries++)
        {
            if (LT9611InputVActive(FxDevice) != 0)
            {
                Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
                      "LT9611 input live after %d re-lock(s)", tries);
                break;
            }
            RubikDelayMs(300);
            LT9611VideoRelock(FxDevice);
        }

        CecHwRestore(FxDevice);
    }
    else
    {
		LT9611HDMIOutputDisable(FxDevice);
    }

    status = STATUS_SUCCESS;

	return status;
}

NTSTATUS LT9611BridgeEnable(WDFDEVICE FxDevice)
{
    NTSTATUS status;

    status = LT9611PowerOn(FxDevice, TRUE);
    if (!NT_SUCCESS(status))
    {
		Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Failed to power on LT9611 %d", status);
        return status;
    }

    status = LT9611VideoOn(FxDevice, TRUE);
    if (!NT_SUCCESS(status))
    {
		Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Failed to enable video %d", status);
        return status;
    }

	return status;
}

#define LT9611_EDID_BLOCK       128
#define LT9611_DDC_ACCS_DONE    0x02
#define LT9611_DDC_ERROR_BITS   0x50
#define LT9611_EDID_POOL_TAG    'dEbL'

static BOOLEAN LT9611EdidBlockValid(const UCHAR *Block)
{
    static const UCHAR header[8] = { 0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00 };
    ULONG i;
    UCHAR sum = 0;

    if (RtlCompareMemory(Block, header, sizeof(header)) != sizeof(header))
    {
        return FALSE;
    }
    for (i = 0; i < LT9611_EDID_BLOCK; i++)
    {
        sum = (UCHAR)(sum + Block[i]);
    }
    return (BOOLEAN)(sum == 0);
}

static VOID LT9611EdidDump(const UCHAR *Edid, ULONG Length)
{
    ULONG off;

    for (off = 0; off + 16 <= Length; off += 16)
    {
        const UCHAR *r = Edid + off;
        Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
              "EDID %02X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
              off,
              r[0], r[1], r[2],  r[3],  r[4],  r[5],  r[6],  r[7],
              r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
    }
}

NTSTATUS LT9611EdidRead(WDFDEVICE FxDevice, UCHAR *Edid, ULONG *BytesRead)
{
    NTSTATUS      status;
    LARGE_INTEGER delay;
    UCHAR         reg = 0;
    ULONG         chunk, i;
    ULONG         chunks = LT9611_EDID_BLOCK / 32;

    *BytesRead = 0;
    RtlZeroMemory(Edid, LT9611_EDID_MAX);

    status = LT9611Write(FxDevice, 0xff, 0x85);
    if (!NT_SUCCESS(status)) { goto done; }

    (VOID)LT9611Write(FxDevice, 0x03, 0xc9);
    (VOID)LT9611Write(FxDevice, 0x04, 0xa0);
    (VOID)LT9611Write(FxDevice, 0x05, 0x00);
    (VOID)LT9611Write(FxDevice, 0x06, 0x20);
    (VOID)LT9611Write(FxDevice, 0x14, 0x7f);

    for (chunk = 0; chunk < chunks; chunk++)
    {
        (VOID)LT9611Write(FxDevice, 0x05, (UCHAR)(chunk * 32));
        (VOID)LT9611Write(FxDevice, 0x07, 0x36);
        (VOID)LT9611Write(FxDevice, 0x07, 0x34);
        (VOID)LT9611Write(FxDevice, 0x07, 0x37);

        delay.QuadPart = -(5 * 10 * 1000);
        KeDelayExecutionThread(KernelMode, FALSE, &delay);

        status = LT9611Read(FxDevice, 0x40, &reg, 1);
        if (!NT_SUCCESS(status)) { goto done; }

        if ((reg & LT9611_DDC_ACCS_DONE) == 0)
        {
            Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                  "EDID: DDC not done at chunk %u (0x40=0x%02X)", chunk, reg);
            status = STATUS_DEVICE_NOT_READY;
            goto done;
        }
        if ((reg & LT9611_DDC_ERROR_BITS) != 0)
        {
            Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                  "EDID: DDC no-ack/arb-lost at chunk %u (0x40=0x%02X)", chunk, reg);
            status = STATUS_DEVICE_PROTOCOL_ERROR;
            goto done;
        }

        for (i = 0; i < 32; i++)
        {
            UCHAR b = 0;
            status = LT9611Read(FxDevice, 0x83, &b, 1);
            if (!NT_SUCCESS(status)) { goto done; }
            Edid[chunk * 32 + i] = b;
        }

        *BytesRead = (chunk + 1) * 32;

        if (chunk == (LT9611_EDID_BLOCK / 32) - 1)
        {
            if (Edid[0x7E] == 0)
            {
                break;
            }
            chunks = LT9611_EDID_MAX / 32;
        }
    }

    status = STATUS_SUCCESS;

done:
    (VOID)LT9611Write(FxDevice, 0xff, 0x80);
    return status;
}

static NTSTATUS LT9611EdidPublish(const UCHAR *Block)
{
    static const WCHAR classPath[] =
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Class"
        L"\\{4d36e968-e325-11ce-bfc1-08002be10318}";

    NTSTATUS          status;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING    us;
    HANDLE            hClass = NULL;
    ULONG             index;
    UCHAR            *buf = NULL;
    ULONG             len;

    len = sizeof(KEY_BASIC_INFORMATION) + 128 * sizeof(WCHAR);
    buf = (UCHAR *)ExAllocatePool2(POOL_FLAG_PAGED, len, LT9611_EDID_POOL_TAG);
    if (buf == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlInitUnicodeString(&us, classPath);
    InitializeObjectAttributes(&oa, &us,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = ZwOpenKey(&hClass, KEY_READ, &oa);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER,
              "EDID: open display class key failed 0x%08X", status);
        goto cleanup;
    }

    status = STATUS_NOT_FOUND;

    for (index = 0; ; index++)
    {
        KEY_BASIC_INFORMATION         *ki = (KEY_BASIC_INFORMATION *)buf;
        KEY_VALUE_PARTIAL_INFORMATION *vi;
        UCHAR           vbuf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 64];
        WCHAR           svc[32];
        UNICODE_STRING  name;
        NTSTATUS        st;
        ULONG           got = 0;
        ULONG           one = 1;
        HANDLE          hInst = NULL;
        HANDLE          hDisp = NULL;

        st = ZwEnumerateKey(hClass, index, KeyBasicInformation, buf, len, &got);
        if (!NT_SUCCESS(st))
        {
            break;
        }

        name.Buffer        = ki->Name;
        name.Length        = (USHORT)ki->NameLength;
        name.MaximumLength = (USHORT)ki->NameLength;

        InitializeObjectAttributes(&oa, &name,
                                   OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                                   hClass, NULL);
        if (!NT_SUCCESS(ZwOpenKey(&hInst, KEY_READ | KEY_WRITE, &oa)))
        {
            continue;
        }

        RtlInitUnicodeString(&us, L"Service");
        vi = (KEY_VALUE_PARTIAL_INFORMATION *)vbuf;
        RtlZeroMemory(svc, sizeof(svc));

        if (NT_SUCCESS(ZwQueryValueKey(hInst, &us, KeyValuePartialInformation,
                                       vbuf, sizeof(vbuf), &got)) &&
            vi->Type == REG_SZ &&
            vi->DataLength > 0)
        {
            ULONG copy = vi->DataLength;
            if (copy > sizeof(svc) - sizeof(WCHAR))
            {
                copy = sizeof(svc) - sizeof(WCHAR);
            }
            RtlCopyMemory(svc, vi->Data, copy);
        }

        if (_wcsicmp(svc, L"QCDX") != 0)
        {
            ZwClose(hInst);
            continue;
        }

        RtlInitUnicodeString(&us, L"DISPLAY");
        InitializeObjectAttributes(&oa, &us,
                                   OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                                   hInst, NULL);
        status = ZwCreateKey(&hDisp, KEY_SET_VALUE, &oa, 0, NULL,
                             REG_OPTION_NON_VOLATILE, NULL);
        if (NT_SUCCESS(status))
        {
            RtlInitUnicodeString(&us, L"PanelEdid");
            status = ZwSetValueKey(hDisp, &us, 0, REG_BINARY,
                                   (PVOID)Block, LT9611_EDID_BLOCK);

            if (NT_SUCCESS(status))
            {
                RtlInitUnicodeString(&us, L"ReadEDIDFromReg");
                status = ZwSetValueKey(hDisp, &us, 0, REG_DWORD,
                                       &one, sizeof(one));
            }

            ZwClose(hDisp);
        }

        ZwClose(hInst);

        Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
              "EDID: published to display driver key, status 0x%08X", status);
        break;
    }

cleanup:
    if (hClass != NULL) { ZwClose(hClass); }
    if (buf != NULL)    { ExFreePoolWithTag(buf, LT9611_EDID_POOL_TAG); }
    return status;
}

NTSTATUS LT9611EdidRefresh(WDFDEVICE FxDevice)
{
    NTSTATUS status;
    ULONG    got = 0;
    UCHAR    edid[LT9611_EDID_MAX];

    status = LT9611EdidRead(FxDevice, edid, &got);
    if (!NT_SUCCESS(status) || got < LT9611_EDID_BLOCK)
    {
        Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER,
              "EDID: read failed (0x%08X, %u bytes) - sink may be absent",
              status, got);
        return NT_SUCCESS(status) ? STATUS_DEVICE_NOT_READY : status;
    }

    LT9611EdidDump(edid, got);

    if (!LT9611EdidBlockValid(edid))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER,
              "EDID: block 0 header/checksum invalid - not publishing");
        return STATUS_INVALID_DEVICE_STATE;
    }

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
          "EDID: valid, %u bytes, %u extension block(s)", got, edid[0x7E]);

    HdmiAudioCacheEdid(FxDevice, edid, got);

    OEMBridgeCacheSinkModes(FxDevice, edid, got);

    return LT9611EdidPublish(edid);
}

#define LT9611_POLL_IDLE_MS         2000
#define LT9611_POLL_WATCH_MS         500
#define LT9611_POLL_RETRY_MS         300
#define LT9611_ESCALATE_REINIT         3
#define LT9611_ESCALATE_RESET          6
#define LT9611_ESCALATE_GIVEUP         7

typedef struct _LT9611_HEALTH
{
    UCHAR   Hpd;
    UINT32  HAct, VAct, HTot;
    BOOLEAN PllLocked;
} LT9611_HEALTH;

#define LT9611_RASTER_TOL_ACT    4
#define LT9611_RASTER_TOL_HTS   16

static BOOLEAN RubikNear(UINT32 a, UINT32 b, UINT32 tol)
{
    return (((a > b) ? (a - b) : (b - a)) <= tol) ? TRUE : FALSE;
}

static BOOLEAN LT9611RasterMatches(PDEVICE_CONTEXT ctx, UINT32 hAct, UINT32 vAct, UINT32 hTot,
                                   UINT32 refHAct, UINT32 refVAct, UINT32 refHTot,
                                   int refMode)
{
    if (RubikNear(hAct, refHAct, LT9611_RASTER_TOL_ACT) &&
        RubikNear(vAct, refVAct, LT9611_RASTER_TOL_ACT) &&
        RubikNear(hTot, refHTot, LT9611_RASTER_TOL_HTS))
    {
        return TRUE;
    }

    return (refMode >= 0 &&
            RubikSelectMode(ctx, hAct, vAct, hTot) == refMode) ? TRUE : FALSE;
}

static BOOLEAN LT9611Sample(WDFDEVICE FxDevice, LT9611_HEALTH *H)
{
    NTSTATUS status;
    UCHAR hpd = 0, lock = 0, cal = 0;

    RtlZeroMemory(H, sizeof(*H));

    status = LT9611Write(FxDevice, 0xff, 0x82);
    if (NT_SUCCESS(status)) { status = LT9611Read(FxDevice, 0x5e, &hpd, 1);  }
    if (NT_SUCCESS(status)) { status = LT9611Read(FxDevice, 0x15, &lock, 1); }
    if (NT_SUCCESS(status)) { status = LT9611Read(FxDevice, 0xe7, &cal, 1);  }
    if (!NT_SUCCESS(status))
    {
        return FALSE;
    }

    H->Hpd       = (hpd & 0x04) ? 1 : 0;
    H->PllLocked = ((lock & 0x80) && (cal & 0x80)) ? TRUE : FALSE;

    RubikMeasureInput(FxDevice, &H->HAct, &H->VAct, &H->HTot);
    return TRUE;
}

VOID LT9611SeedHealthState(WDFDEVICE FxDevice)
{
    PDEVICE_CONTEXT devContext = DeviceGetContext(FxDevice);
    LT9611_HEALTH h;

    devContext->RelockRetries = 0;

    if (!LT9611Sample(FxDevice, &h))
    {
        devContext->LastHpd       = 0;
        devContext->LastInputLive = 0;
        devContext->LockedHAct    = 0;
        devContext->LockedVAct    = 0;
        devContext->LockedHTot    = 0;
        devContext->LockedMode    = -1;
        devContext->RasterPending = 0;
        Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER,
              "health: seed failed - bridge not answering");
        return;
    }

    devContext->LastHpd       = h.Hpd;
    devContext->LastInputLive = (h.VAct != 0) ? 1 : 0;
    devContext->LockedHAct    = h.HAct;
    devContext->LockedVAct    = h.VAct;
    devContext->LockedHTot    = h.HTot;
    devContext->LockedMode    = RubikSelectMode(devContext, h.HAct, h.VAct, h.HTot);
    devContext->RasterPending = 0;

    HdmiAudioSetSinkPresent(FxDevice, h.Hpd ? TRUE : FALSE);

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
          "health: seeded hpd=%d input %dx%d hts=%d pll=%d",
          h.Hpd, h.HAct, h.VAct, h.HTot, h.PllLocked);
}

static VOID LT9611RecoveryStep(WDFDEVICE FxDevice, BOOLEAN ChipSilent)
{
    PDEVICE_CONTEXT devContext = DeviceGetContext(FxDevice);

    devContext->RelockRetries++;

    if (devContext->RelockRetries == LT9611_ESCALATE_REINIT)
    {

        Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER,
              "health: escalating to full bring-up");
        devContext->PowerOn = FALSE;
        LT9611BridgeEnable(FxDevice);
    }
    else if (devContext->RelockRetries == LT9611_ESCALATE_RESET)
    {
        if (ChipSilent)
        {

            Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                  "health: bridge silent - hard reset + bring-up");
            LT9611Reset(FxDevice, TRUE);
        }
        else
        {

            Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                  "health: chip answers - skipping hard reset, repeating bring-up");
        }
        devContext->PowerOn = FALSE;
        LT9611BridgeEnable(FxDevice);
    }
    else if (devContext->RelockRetries == LT9611_ESCALATE_GIVEUP)
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER,
              "health: recovery exhausted - idling until something changes");
    }
}

VOID LT9611RelockVerified(WDFDEVICE FxDevice, PCSTR Reason)
{
    PDEVICE_CONTEXT devContext = DeviceGetContext(FxDevice);
    LT9611_HEALTH h;
    int mode = -1;

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
          "health: %s -> re-lock", Reason);
    LT9611VideoRelockEx(FxDevice, &mode);

    if (!LT9611Sample(FxDevice, &h))
    {
        LT9611RecoveryStep(FxDevice, TRUE);
        return;
    }

    if (mode >= 0 && h.VAct != 0)
    {

        if (!h.PllLocked && !devContext->PllStatusBad)
        {
            devContext->PllStatusBad = 1;
            Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                  "health: Tx-PLL status unreliable - ignoring it from now on");
        }

        devContext->LastHpd       = h.Hpd;
        devContext->LastInputLive = 1;
        devContext->LockedHAct    = h.HAct;
        devContext->LockedVAct    = h.VAct;
        devContext->LockedHTot    = h.HTot;
        devContext->LockedMode    = mode;
        devContext->RasterPending = 0;

        if (devContext->RelockRetries != 0)
        {
            Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                  "health: recovered (%s), input %dx%d hts=%d mode %d after %d failure(s)",
                  Reason, h.HAct, h.VAct, h.HTot, mode, devContext->RelockRetries);
        }
        devContext->RelockRetries = 0;

        Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
              "health: locked %dx%d hts=%d mode %d",
              h.HAct, h.VAct, h.HTot, mode);
        return;
    }

    devContext->LastHpd       = h.Hpd;
    devContext->LastInputLive = (h.VAct != 0) ? 1 : 0;
    devContext->RasterPending = 0;
    Trace(TRACE_LEVEL_WARNING, TRACE_DRIVER,
          "health: %s: re-lock did not verify (hpd %d input %dx%d hts=%d mode %d) - attempt %d",
          Reason, h.Hpd, h.HAct, h.VAct, h.HTot, mode, devContext->RelockRetries + 1);
    LT9611RecoveryStep(FxDevice, FALSE);
}

VOID OnHealthTimer(WDFTIMER Timer)
{
    WDFDEVICE device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PDEVICE_CONTEXT devContext = DeviceGetContext(device);
    LT9611_HEALTH h;
    BOOLEAN sampled;
    ULONG interval;

    WdfWaitLockAcquire(devContext->BridgeLock, NULL);

    if (!devContext->BridgeReady)
    {

        WdfWaitLockRelease(devContext->BridgeLock);
        return;
    }

    sampled = LT9611Sample(device, &h);

    if (devContext->RelockRetries >= LT9611_ESCALATE_GIVEUP)
    {

        if (!sampled ||
            (h.Hpd == devContext->LastHpd &&
             ((h.VAct != 0) ? 1 : 0) == devContext->LastInputLive))
        {
            goto rearm;
        }
        Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
              "health: transition observed - recovery re-armed");
        devContext->RelockRetries = 0;
    }

    if (!sampled)
    {

        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER,
              "health: bridge not answering I2C (attempt %d)",
              devContext->RelockRetries + 1);
        LT9611RecoveryStep(device, TRUE);
        goto rearm;
    }

    if (h.Hpd && !devContext->LastHpd)
    {

        (VOID)LT9611EdidRefresh(device);

        LT9611RelockVerified(device, "sink connected");

        HdmiAudioSetSinkPresent(device, TRUE);
    }
    else if (!h.Hpd && devContext->LastHpd)
    {
        Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
              "health: sink disconnected -> output off");
        LT9611HDMIOutputDisable(device);
        devContext->LastHpd       = 0;
        devContext->LastInputLive = (h.VAct != 0) ? 1 : 0;
        devContext->RelockRetries = 0;

        HdmiAudioSetSinkPresent(device, FALSE);
    }
    else if (h.Hpd && h.VAct == 0 && devContext->LastInputLive)
    {

        Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
              "health: input died -> output off, waiting quietly");
        LT9611HDMIOutputDisable(device);
        devContext->LastInputLive = 0;
        devContext->RelockRetries = 0;
    }
    else if (h.Hpd && h.VAct != 0 && !devContext->LastInputLive)
    {
        LT9611RelockVerified(device, "input came alive");
    }
    else if (h.Hpd && devContext->RelockRetries != 0)
    {

        LT9611RelockVerified(device, "pending recovery");
    }
    else if (h.Hpd && h.VAct != 0 &&
             !LT9611RasterMatches(devContext, h.HAct, h.VAct, h.HTot,
                                  devContext->LockedHAct,
                                  devContext->LockedVAct,
                                  devContext->LockedHTot,
                                  devContext->LockedMode))
    {

        if (devContext->RasterPending &&
            LT9611RasterMatches(devContext, h.HAct, h.VAct, h.HTot,
                                devContext->PendHAct,
                                devContext->PendVAct,
                                devContext->PendHTot,
                                -1))
        {
            Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
                  "health: raster %dx%d hts=%d (was %dx%d hts=%d mode %d)",
                  h.HAct, h.VAct, h.HTot,
                  devContext->LockedHAct, devContext->LockedVAct,
                  devContext->LockedHTot, devContext->LockedMode);
            LT9611RelockVerified(device, "raster changed");
        }
        else
        {
            Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
                  "health: raster %dx%d hts=%d != locked %dx%d hts=%d "
                  "mode %d - confirming",
                  h.HAct, h.VAct, h.HTot,
                  devContext->LockedHAct, devContext->LockedVAct,
                  devContext->LockedHTot, devContext->LockedMode);
            devContext->RasterPending = 1;
            devContext->PendHAct      = h.HAct;
            devContext->PendVAct      = h.VAct;
            devContext->PendHTot      = h.HTot;
        }
    }
    else if (h.Hpd && h.VAct != 0 && !h.PllLocked && !devContext->PllStatusBad)
    {

        LT9611RelockVerified(device, "Tx PLL unlocked");
    }
    else
    {

        devContext->LastHpd       = h.Hpd;
        devContext->LastInputLive = (h.VAct != 0) ? 1 : 0;
        devContext->RasterPending = 0;
        devContext->RelockRetries = 0;
    }

rearm:

    if ((devContext->RelockRetries != 0 &&
         devContext->RelockRetries < LT9611_ESCALATE_GIVEUP) ||
        devContext->RasterPending)
    {
        interval = LT9611_POLL_RETRY_MS;
    }
    else if (devContext->LastHpd && !devContext->LastInputLive)
    {
        interval = LT9611_POLL_WATCH_MS;
    }
    else
    {
        interval = LT9611_POLL_IDLE_MS;
    }

    if (devContext->BridgeReady)
    {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(interval));
    }

    WdfWaitLockRelease(devContext->BridgeLock);
}

static NTSTATUS
LT9611BindResources(WDFDEVICE Device, WDFCMRESLIST Resources)
{
    PDEVICE_CONTEXT context = DeviceGetContext(Device);
    LARGE_INTEGER soleGpio = { 0 };
    ULONG gpioCount = 0;
    BOOLEAN haveI2c = FALSE;
    NTSTATUS status;
    ULONG count = WdfCmResourceListGetCount(Resources);

    for (ULONG index = 0; index < count; ++index)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR descriptor =
            WdfCmResourceListGetDescriptor(Resources, index);
        LARGE_INTEGER *destination = NULL;

        if (descriptor->Type != CmResourceTypeConnection)
        {
            continue;
        }

        switch (descriptor->u.Connection.Class)
        {
        case CM_RESOURCE_CONNECTION_CLASS_SERIAL:
            if (descriptor->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)
            {
                destination = &context->I2CContext.I2cResHubId;
                haveI2c = TRUE;
            }
            break;

        case CM_RESOURCE_CONNECTION_CLASS_GPIO:
            if (descriptor->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_GPIO_IO)
            {
                switch (gpioCount++)
                {
                case 0: destination = &soleGpio; break;
                case 1: destination = &context->ResetGpioId; break;
                default: break;
                }
            }
            break;

        default:
            break;
        }

        if (destination != NULL)
        {
            destination->LowPart = descriptor->u.Connection.IdLowPart;
            destination->HighPart = descriptor->u.Connection.IdHighPart;
        }
    }

    if (!haveI2c || gpioCount == 0 || gpioCount > 2)
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_INIT,
              "LT9611 connections unavailable (I2C %lu, GPIO count %lu)",
              (ULONG)haveI2c, gpioCount);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (gpioCount == 1)
    {
        context->ResetGpioId = soleGpio;
    }

    status = OpenIOTarget(context, context->ResetGpioId,
                          GENERIC_READ | GENERIC_WRITE, &context->ResetGpio);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER,
              "LT9611 reset connection failed - 0x%08lX", status);
        return status;
    }

    status = SpbTargetInitialize(Device, &context->I2CContext);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_INIT,
              "LT9611 I2C connection failed - 0x%08lX", status);
    }
    return status;
}

static NTSTATUS
LT9611TakeOverHardware(WDFDEVICE FxDevice)
{
    NTSTATUS status;

    UCHAR rev = 0;

    LT9611Write(FxDevice, 0xff, 0x80);
    LT9611Write(FxDevice, 0xee, 0x01);
    status = LT9611Read(FxDevice, 0x02, &rev, 1);
    if (NT_SUCCESS(status) && rev != 0)
    {
        Trace(TRACE_LEVEL_INFORMATION, TRACE_INIT,
              "LT9611 warm takeover, revision: 0x%x", rev);
    }
    else
    {
        int attempt;
        Trace(TRACE_LEVEL_WARNING, TRACE_INIT,
              "LT9611 not answering warm (st=0x%08lX rev=0x%x), cold reset",
              status, rev);

        LT9611Reset(FxDevice, TRUE);

        for (attempt = 0; attempt < 10; attempt++)
        {
            LT9611Write(FxDevice, 0xff, 0x80);
            LT9611Write(FxDevice, 0xee, 0x01);
            status = LT9611Read(FxDevice, 0x02, &rev, 1);
            if (NT_SUCCESS(status) && rev != 0)
            {
                break;
            }
            RubikDelayMs(50);
        }
        Trace(TRACE_LEVEL_INFORMATION, TRACE_INIT,
              "LT9611 cold rev=0x%x after %d attempts (st=0x%08lX)",
              rev, attempt + 1, status);
    }

    return status;
}

NTSTATUS
OnPrepareHardware(
    IN WDFDEVICE FxDevice,
    IN WDFCMRESLIST FxResourcesRaw,
    IN WDFCMRESLIST FxResourcesTranslated
)
{
    PDEVICE_CONTEXT devContext = DeviceGetContext(FxDevice);
    NTSTATUS status;

    UNREFERENCED_PARAMETER(FxResourcesRaw);

    status = LT9611BindResources(FxDevice, FxResourcesTranslated);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = LT9611TakeOverHardware(FxDevice);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_INIT,
              "LT9611 revision read failed - 0x%08lX", status);
        return status;
    }

    HdmiAudioLoadSettings(FxDevice);
    CecPrepare(FxDevice);
    status = LT9611BridgeEnable(FxDevice);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Failed to enable LT9611 bridge %d", status);
        return status;
    }

    devContext->BridgeReady = TRUE;

    (VOID)LT9611EdidRefresh(FxDevice);

    LT9611SeedHealthState(FxDevice);

    CecChildSetPresent(FxDevice, devContext->Cec.Enabled);

    NTSTATUS muxStatus = LpiMuxPrepare(FxDevice, FxResourcesTranslated);
    if (!NT_SUCCESS(muxStatus))
    {
        Trace(TRACE_LEVEL_WARNING, TRACE_INIT,
              "LPI mux prepare failed - 0x%08lX (HDMI audio will be silent)", muxStatus);
    }

    {
        NTSTATUS haStatus = HdmiAudioPrepare(FxDevice);
        if (!NT_SUCCESS(haStatus))
        {
            Trace(TRACE_LEVEL_WARNING, TRACE_INIT,
                  "HDMI audio provider prepare failed - 0x%08lX (no ExternalDisplay endpoint)", haStatus);
        }
    }

    return status;
}

NTSTATUS
OnD0Entry(
    IN WDFDEVICE Device,
    IN WDF_POWER_DEVICE_STATE PreviousState
)

{
    NTSTATUS status;
    PDEVICE_CONTEXT devContext;

    devContext = DeviceGetContext(Device);

    UNREFERENCED_PARAMETER(PreviousState);

    WdfWaitLockAcquire(devContext->BridgeLock, NULL);

    if (!devContext->InitialD0Done)
    {

        devContext->InitialD0Done = TRUE;
    }
    else
    {

        Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "D0Entry: resume re-init");
        devContext->PowerOn = FALSE;
        LT9611PowerOn(Device, TRUE);
        LT9611VideoOn(Device, TRUE);
    }
    devContext->BridgeReady = TRUE;

    LT9611SeedHealthState(Device);

    WdfWaitLockRelease(devContext->BridgeLock);

    if (devContext->HealthTimer != NULL)
    {
        WdfTimerStart(devContext->HealthTimer, WDF_REL_TIMEOUT_IN_MS(2000));
    }

    CecStart(Device);

    status = STATUS_SUCCESS;

    return status;
}

NTSTATUS
OnD0Exit(
    IN WDFDEVICE Device,
    IN WDF_POWER_DEVICE_STATE TargetState
)

{
    NTSTATUS status;
    PDEVICE_CONTEXT devContext;

    PAGED_CODE();

    devContext = DeviceGetContext(Device);

    UNREFERENCED_PARAMETER(TargetState);

    WdfWaitLockAcquire(devContext->BridgeLock, NULL);
    devContext->BridgeReady = FALSE;
    WdfWaitLockRelease(devContext->BridgeLock);

    CecStop(Device);

    if (devContext->HealthTimer != NULL)
    {
        WdfTimerStop(devContext->HealthTimer, TRUE);
    }

    status = STATUS_SUCCESS;

    return status;
}

NTSTATUS
OnReleaseHardware(
    IN WDFDEVICE FxDevice,
    IN WDFCMRESLIST FxResourcesTranslated
)
{
    PDEVICE_CONTEXT context = DeviceGetContext(FxDevice);
    WDFIOTARGET reset;

    UNREFERENCED_PARAMETER(FxResourcesTranslated);

    HdmiAudioRelease(FxDevice);
    LpiMuxRelease(FxDevice);

    reset = context->ResetGpio;
    if (reset != NULL)
    {
        WdfIoTargetClose(reset);
        WdfObjectDelete(reset);
    }
    context->ResetGpio = NULL;
    context->ResetGpioId.QuadPart = 0;

    SpbTargetDeinitialize(FxDevice, &context->I2CContext);
    context->I2CContext.I2cResHubId.QuadPart = 0;

    return STATUS_SUCCESS;
}

static NTSTATUS
LT9611CreateMonitor(WDFDEVICE Device)
{
    PDEVICE_CONTEXT context = DeviceGetContext(Device);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_TIMER_CONFIG timer;
    NTSTATUS status;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;
    status = WdfWaitLockCreate(&attributes, &context->BridgeLock);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_INIT,
              "LT9611 bridge lock creation failed - 0x%08lX", status);
        return status;
    }

    WDF_TIMER_CONFIG_INIT(&timer, OnHealthTimer);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfTimerCreate(&timer, &attributes, &context->HealthTimer);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_INIT,
              "LT9611 health timer creation failed - 0x%08lX", status);
    }
    return status;
}

NTSTATUS
OEMBridgeCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_PNPPOWER_EVENT_CALLBACKS callbacks;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFDEVICE device;
    PDEVICE_CONTEXT context;
    NTSTATUS status;

    PAGED_CODE();

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&callbacks);
    callbacks.EvtDevicePrepareHardware = OnPrepareHardware;
    callbacks.EvtDeviceReleaseHardware = OnReleaseHardware;
    callbacks.EvtDeviceD0Entry = OnD0Entry;
    callbacks.EvtDeviceD0Exit = OnD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &callbacks);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    CecChildInitList(DeviceInit);

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_INIT,
              "WdfDeviceCreate failed - 0x%08lX", status);
        return status;
    }

    OEMBridgeLoadLoggingSettings(device);
    context = DeviceGetContext(device);
    context->FxDevice = device;
    context->LockedMode = -1;

    status = LT9611CreateMonitor(device);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    {
        NTSTATUS muxStatus = LpiMuxCreate(device);
        if (!NT_SUCCESS(muxStatus))
        {
            Trace(TRACE_LEVEL_WARNING, TRACE_INIT,
                  "LPI mux objects unavailable - 0x%08lX", muxStatus);
        }
    }

    {
        NTSTATUS haStatus = HdmiAudioCreate(device);
        if (!NT_SUCCESS(haStatus))
        {
            Trace(TRACE_LEVEL_WARNING, TRACE_INIT,
                  "HDMI audio provider objects unavailable - 0x%08lX", haStatus);
        }
    }

    {
        NTSTATUS cecStatus = CecCreate(device);
        if (!NT_SUCCESS(cecStatus))
        {
            Trace(TRACE_LEVEL_WARNING, TRACE_INIT,
                  "CEC objects unavailable - 0x%08lX", cecStatus);
        }
    }

    {
        NTSTATUS qdiStatus = QdiBridgeInitialize(device);
        if (!NT_SUCCESS(qdiStatus))
        {
            Trace(TRACE_LEVEL_WARNING, TRACE_INIT,
                  "QDI bridge registration unavailable - 0x%08lX", qdiStatus);
        }
    }

    return status;
}
