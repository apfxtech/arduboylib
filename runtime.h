#pragma once

#include <stddef.h>
#include <stdint.h>

#include <furi.h>

#include "Arduboy2.h"

extern uint8_t* buf;

uint16_t time_ms(void);
uint8_t poll_btns(void);

Arduboy2Base* arduboy_runtime_bridge(void);

void setup(void);
void loop(void);
