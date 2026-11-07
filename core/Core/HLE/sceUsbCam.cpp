// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

// The PSP's camera: the Brick has none, so a game's calls go unanswered.
#include "Common/CommonTypes.h"
#include "Common/Serialize/Serializer.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/sceUsbCam.h"

void __UsbCamInit() {
}

void __UsbCamDoState(PointerWrap &p) {
	auto s = p.Section("sceUsbCam", 1);
}

void __UsbCamShutdown() {
}

const HLEFunction sceUsbCam[] =
{
	{0X03ED7A82, nullptr, "sceUsbCamSetupMic",                       'i', "xxi" },
	{ 0X2E930264, nullptr,                                    "sceUsbCamSetupMicEx",                     '?', "" },
	{0X82A64030, nullptr, "sceUsbCamStartMic",                       'i', "" },
	{0X5145868A, nullptr, "sceUsbCamStopMic",                        'i', "" },
	{0X36636925, nullptr, "sceUsbCamReadMicBlocking",                'i', "xx" },
	{0X3DC0088E, nullptr, "sceUsbCamReadMic",                        'i', "xx" },
	{ 0XB048A67D, nullptr,                                    "sceUsbCamWaitReadMicEnd",                 '?', "" },
	{ 0XF8847F60, nullptr,                                    "sceUsbCamPollReadMicEnd",                 '?', "" },
	{0X5778B452, nullptr, "sceUsbCamGetMicDataLength",               'i', "" },
	{ 0X08AEE98A, nullptr,                                    "sceUsbCamSetMicGain",                     '?', "" },

	{0X17F7B2FB, nullptr, "sceUsbCamSetupVideo",                     'i', "xxi" },
	{0XCFE9E999, nullptr, "sceUsbCamSetupVideoEx",                   'i', "xxi" },
	{0X574A8C3F, nullptr, "sceUsbCamStartVideo",                     'i', "" },
	{0X6CF32CB9, nullptr, "sceUsbCamStopVideo",                      'i', "" },
	{0X7DAC0C71, nullptr, "sceUsbCamReadVideoFrameBlocking",         'i', "xx" },
	{0X99D86281, nullptr, "sceUsbCamReadVideoFrame",                 'i', "xx" },
	{ 0XF90B2293, nullptr,                                    "sceUsbCamWaitReadVideoFrameEnd",          '?', "" },
	{0X41E73E95, nullptr, "sceUsbCamPollReadVideoFrameEnd",          'i', "" },
	{ 0XDF9D0C92, nullptr,                                    "sceUsbCamGetReadVideoFrameSize",          '?', "" },

	{0X3F0CF289, nullptr, "sceUsbCamSetupStill",                     'i', "x" },
	{0X0A41A298, nullptr, "sceUsbCamSetupStillEx",                   'i', "x" },
	{ 0X61BE5CAC, nullptr,                                    "sceUsbCamStillInputBlocking",             '?', "" },
	{ 0XFB0A6C5D, nullptr,                                    "sceUsbCamStillInput",                     '?', "" },
	{ 0X7563AFA1, nullptr,                                    "sceUsbCamStillWaitInputEnd",              '?', "" },
	{ 0X1A46CFE7, nullptr,                                    "sceUsbCamStillPollInputEnd",              '?', "" },
	{ 0XA720937C, nullptr,                                    "sceUsbCamStillCancelInput",               '?', "" },
	{ 0XE5959C36, nullptr,                                    "sceUsbCamStillGetInputLength",            '?', "" },

	{0XF93C4669, nullptr, "sceUsbCamAutoImageReverseSW",             'i', "i" },
	{ 0X11A1F128, nullptr,                                    "sceUsbCamGetAutoImageReverseState",       '?', "" },
	{0X4C34F553, nullptr, "sceUsbCamGetLensDirection",               'i', "" },

	{ 0X383E9FA8, nullptr,                                    "sceUsbCamGetSaturation",                  '?', "" },
	{ 0X6E205974, nullptr,                                    "sceUsbCamSetSaturation",                  '?', "" },
	{ 0X70F522C5, nullptr,                                    "sceUsbCamGetBrightness",                  '?', "" },
	{ 0X4F3D84D5, nullptr,                                    "sceUsbCamSetBrightness",                  '?', "" },
	{ 0XA063A957, nullptr,                                    "sceUsbCamGetContrast",                    '?', "" },
	{ 0X09C26C7E, nullptr,                                    "sceUsbCamSetContrast",                    '?', "" },
	{ 0XFDB68C23, nullptr,                                    "sceUsbCamGetSharpness",                   '?', "" },
	{ 0X622F83CC, nullptr,                                    "sceUsbCamSetSharpness",                   '?', "" },
	{ 0X994471E0, nullptr,                                    "sceUsbCamGetImageEffectMode",             '?', "" },
	{ 0XD4876173, nullptr,                                    "sceUsbCamSetImageEffectMode",             '?', "" },
	{ 0X2BCD50C0, nullptr,                                    "sceUsbCamGetEvLevel",                     '?', "" },
	{ 0X1D686870, nullptr,                                    "sceUsbCamSetEvLevel",                     '?', "" },
	{ 0XD5279339, nullptr,                                    "sceUsbCamGetReverseMode",                 '?', "" },
	{0X951BEDF5, nullptr, "sceUsbCamSetReverseMode",                 'i', "i" },
	{ 0X9E8AAF8D, nullptr,                                    "sceUsbCamGetZoom",                        '?', "" },
	{ 0XC484901F, nullptr,                                    "sceUsbCamSetZoom",                        '?', "" },
	{ 0XAA7D94BA, nullptr,                                    "sceUsbCamGetAntiFlicker",                 '?', "" },
	{ 0X6784E6A8, nullptr,                                    "sceUsbCamSetAntiFlicker",                 '?', "" },

	{ 0XD293A100, nullptr,                                    "sceUsbCamRegisterLensRotationCallback",   '?', "" },
	{ 0X41EE8797, nullptr,                                    "sceUsbCamUnregisterLensRotationCallback", '?', "" },
};

void Register_sceUsbCam()
{
	RegisterHLEModule("sceUsbCam", ARRAY_SIZE(sceUsbCam), sceUsbCam);
}
