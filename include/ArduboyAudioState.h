#pragma once

#include <stdint.h>
#include <furi.h>

typedef struct {
    const uint16_t* pattern;
} ArduboyToneSoundRequest;

extern FuriMessageQueue* g_arduboy_sound_queue;
extern FuriThread* g_arduboy_sound_thread;
extern volatile bool g_arduboy_sound_thread_running;
extern volatile bool g_arduboy_audio_enabled;
extern volatile bool g_arduboy_tones_playing;
extern volatile uint8_t g_arduboy_volume_mode;
extern volatile bool g_arduboy_force_high;
extern volatile bool g_arduboy_force_norm;
