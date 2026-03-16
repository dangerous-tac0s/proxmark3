//-----------------------------------------------------------------------------
// Copyright (C) Jonathan Westhues, Aug 2005
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------
// Utility functions used in many places, not specific to any piece of code.
//-----------------------------------------------------------------------------

#ifndef __UTIL_H
#define __UTIL_H

#include "common.h"

// PRIx64 definition missing with gcc-arm-none-eabi v8?
#ifndef PRIx64
#define PRIx64 "llx"
#endif

// Basic macros

#ifndef SHORT_COIL
#define SHORT_COIL()     LOW(GPIO_SSC_DOUT)
#endif

#ifndef OPEN_COIL
#define OPEN_COIL()      HIGH(GPIO_SSC_DOUT)
#endif

#ifndef BYTEx
#define BYTEx(x, n) (((x) >> (n * 8)) & 0xff )
#endif

// Proxmark3 RDV4.0 and Proxmark Easy LEDs
#define LED_A 1
#define LED_B 2
#define LED_C 4
#define LED_D 8


#ifndef LED_ORDER_PM3EASY
// Proxmark3 historical LEDs
#define LED_ORANGE LED_A
#define LED_GREEN  LED_B
#define LED_RED    LED_C
#define LED_RED2   LED_D
#else
// Proxmark3 Easy LEDs
#define LED_GREEN  LED_A
#define LED_RED    LED_B
#define LED_ORANGE LED_C
#define LED_RED2   LED_D
#endif

// Tune LED assignments (must be PWM-capable: PA0/PWM0 or PA2/PWM2)
#ifndef LED_ORDER_PM3EASY
// RDV4: LF=Red2 (LED_D/PA2/PWM2), HF=Orange (LED_A/PA0/PWM0)
#define LED_LF_TUNE LED_D
#define LED_HF_TUNE LED_A
#else
// PM3 Easy: LF=Green (LED_A/PA0/PWM0), HF=Red (LED_B/PA2/PWM2)
#define LED_LF_TUNE LED_A
#define LED_HF_TUNE LED_B
#endif

#define BUTTON_HOLD 1
#define BUTTON_NO_CLICK 0
#define BUTTON_SINGLE_CLICK -1
#define BUTTON_DOUBLE_CLICK -2
#define BUTTON_ERROR -99


#ifndef BIT32
#define BIT32(x,n)      ((((x)[(n)>>5])>>((n)))&1)
#endif

#ifndef INV32
#define INV32(x,i,n)    ((x)[(i)>>5]^=((uint32_t)(n))<<((i)&31))
#endif

#ifndef ROTL64
#define ROTL64(x, n)    ((((uint64_t)(x))<<((n)&63))+(((uint64_t)(x))>>((0-(n))&63)))
#endif

size_t nbytes(size_t nbits);
uint8_t hex2int(char x);

int hex2binarray(char *target, const char *source);
int hex2binarray_n(char *target, const char *source, int sourcelen);
int binarray2hex(const uint8_t *bs, int bs_len, uint8_t *hex);

void convertToHexArray(uint32_t num, uint8_t *partialkey);

void LED(int led, int ms);
void LEDsoff(void);
void SpinOff(uint32_t pause);
void SpinErr(uint8_t led, uint32_t speed, uint8_t times);
void SpinDown(uint32_t speed);
void SpinUp(uint32_t speed);

// PWM LED brightness control (PA0/PWM0 = LED_A, PA2/PWM2 = LED_B or LED_D)
void led_pwm_init(void);
void led_set_pwm_brightness(uint8_t led, uint8_t brightness);
void led_pwm_disable(uint8_t led);
void led_effect_pulse(uint8_t led, uint16_t speed_ms, uint16_t count);
void led_effect_fade(uint8_t led, uint16_t speed_ms);
void led_effect_blink(uint8_t led_mask, uint16_t speed_ms, uint16_t count);

int BUTTON_CLICKED(int ms);
int BUTTON_HELD(int ms);
bool data_available(void);
bool data_available_fast(void);

uint32_t flash_size_from_cidr(uint32_t cidr);
uint32_t get_flash_size(void);

#endif
