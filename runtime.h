#pragma once

#include <stddef.h>
#include <stdint.h>

#include <furi.h>

#include "Arduboy2.h"

extern uint8_t* buf;

uint16_t time_ms(void);
uint8_t poll_btns(void);

bool arduboy_screen_inverted(void);
void arduboy_screen_invert(bool invert);
void arduboy_screen_invert_toggle(void);

void setup(void);
void loop(void);
