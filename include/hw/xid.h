#ifndef __HW_XID_H__
#define __HW_XID_H__

//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "qemu/compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XIDGamepadReport {
    uint8_t bReportId;
    uint8_t bLength;
    uint16_t wButtons;
    uint8_t bAnalogButtons[8];
    int16_t sThumbLX;
    int16_t sThumbLY;
    int16_t sThumbRX;
    int16_t sThumbRY;
} QEMU_PACKED XIDGamepadReport;

typedef struct XIDGamepadOutputReport {
    uint8_t report_id; // FIXME: is this correct?
    uint8_t length;
    uint16_t left_actuator_strength;
    uint16_t right_actuator_strength;
} QEMU_PACKED XIDGamepadOutputReport;

typedef struct XIDSteelBattalionReport {
    uint8_t bReportId;
    uint8_t bLength;
    uint32_t dwButtons;
    uint8_t bMoreButtons;
    uint16_t wPadding;
    uint8_t bAimingX;
    uint8_t bPadding;
    uint8_t bAimingY;
    int16_t sRotationLever; // only high byte is used
    int16_t sSightChangeX; // only high byte is used
    int16_t sSightChangeY; // only high byte is used
    uint16_t wLeftPedal; // only high byte is used
    uint16_t wMiddlePedal; // only high byte is used
    uint16_t wRightPedal; // only high byte is used
    uint8_t ucTunerDial; // low nibble, The 9 o'clock postion is 0, and the 6
                         // o'clock position is 12
    uint8_t ucGearLever; // gear lever 1~5 for gear 1~5, 7~13 for gear R,N,1~5,
                         // 15 for gear R
} QEMU_PACKED XIDSteelBattalionReport;

// Based on:
// https://github.com/Ryzee119/ogx360/blob/master/Firmware/src/usbd/usbd_xid.h:195
typedef struct XIDSteelBattalionOutputReport {
    uint8_t report_id;
    uint8_t length;
    uint8_t EmergencyEject : 4;
    uint8_t CockpitHatch : 4;
    uint8_t Ignition : 4;
    uint8_t Start : 4;
    uint8_t OpenClose : 4;
    uint8_t MapZoomInOut : 4;
    uint8_t ModeSelect : 4;
    uint8_t SubMonitorModeSelect : 4;
    uint8_t MainMonitorZoomIn : 4;
    uint8_t MainMonitorZoomOut : 4;
    uint8_t ForecastShootingSystem : 4;
    uint8_t Manipulator : 4;
    uint8_t LineColorChange : 4;
    uint8_t Washing : 4;
    uint8_t Extinguisher : 4;
    uint8_t Chaff : 4;
    uint8_t TankDetach : 4;
    uint8_t Override : 4;
    uint8_t NightScope : 4;
    uint8_t F1 : 4;
    uint8_t F2 : 4;
    uint8_t F3 : 4;
    uint8_t MainWeaponControl : 4;
    uint8_t SubWeaponControl : 4;
    uint8_t MagazineChange : 4;
    uint8_t Comm1 : 4;
    uint8_t Comm2 : 4;
    uint8_t Comm3 : 4;
    uint8_t Comm4 : 4;
    uint8_t Comm5 : 4;
    uint8_t : 4;
    uint8_t GearR : 4;
    uint8_t GearN : 4;
    uint8_t Gear1 : 4;
    uint8_t Gear2 : 4;
    uint8_t Gear3 : 4;
    uint8_t Gear4 : 4;
    uint8_t Gear5 : 4;
    uint8_t not_used;
} QEMU_PACKED XIDSteelBattalionOutputReport;

// Macro for accessing the high nibble of a byte, useful for reading the Steel
// Battalion controller output data
#define HiNibble(x) ((0xF0 & x) >> 4)

// Macro for accessing the low nibble of a byte, useful for reading the Steel
// Battalion controller output data
#define LoNibble(x) (0x0F & x)

#define GAMEPAD_A 0
#define GAMEPAD_B 1
#define GAMEPAD_X 2
#define GAMEPAD_Y 3
#define GAMEPAD_BLACK 4
#define GAMEPAD_WHITE 5
#define GAMEPAD_LEFT_TRIGGER 6
#define GAMEPAD_RIGHT_TRIGGER 7

#define GAMEPAD_DPAD_UP 8
#define GAMEPAD_DPAD_DOWN 9
#define GAMEPAD_DPAD_LEFT 10
#define GAMEPAD_DPAD_RIGHT 11
#define GAMEPAD_START 12
#define GAMEPAD_BACK 13
#define GAMEPAD_LEFT_THUMB 14
#define GAMEPAD_RIGHT_THUMB 15

#define BUTTON_MASK(button) (1 << ((button)-GAMEPAD_DPAD_UP))

#ifdef __cplusplus
}
#endif

#endif