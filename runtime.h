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

void arduboy_runtime_on_begin(
    uint8_t* screen_buffer,
    volatile uint8_t* input_state,
    FuriMutex* game_mutex,
    volatile bool* exit_requested);

Arduboy2Base* arduboy_runtime_primary_arduboy(void);
Arduboy2Base::InputContext* arduboy_runtime_primary_input_context(void);
InputKey arduboy_runtime_map_input_key(InputKey key);
uint8_t arduboy_runtime_map_buttons(uint8_t buttons);
uint8_t arduboy_runtime_transform_byte(uint8_t value, size_t index);
uint32_t arduboy_runtime_fps(void);
void arduboy_runtime_idle(void);
void arduboy_runtime_on_stop(void);
