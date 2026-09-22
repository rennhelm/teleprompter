// tp_board.h - the bits of Waveshare board bring-up we actually need.
//
// The backlight and the panel/touch reset lines hang off an I2C IO expander at
// 0x24, so the expander has to come up before the RGB panel does. Touch is not
// used, but GPIO4 still gets driven through the vendor's power-up sequence
// because that is what puts the expander and the panel rails in a known state.
#pragma once

#include <stdbool.h>
#include <stdint.h>

bool tp_board_init(void);

void tp_board_backlight(bool on);

// 0-100, where 100 is brightest. The expander's PWM register itself is
// inverted; this function hides that. Floored at 10 so the panel can never be
// driven fully dark.
void tp_board_set_brightness(uint8_t percent);
