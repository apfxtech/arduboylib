#pragma once

#include <furi.h>
#include <furi_hal.h>
#include "ArduboyTones.h"
#ifdef ARDULIB_USE_ATM
#include "ATMlib.h"
#endif

class ArduboyAudio {
public:
    void begin() {
        const bool system_enabled = systemAudioEnabled();
        if(system_enabled) {
            on();
        } else {
            off();
        }
    }

    void on() {
        g_arduboy_audio_enabled = true;
        ardulib_tone_init();
#ifdef ARDULIB_USE_ATM
        ardulib_atm_system_init();
        ardulib_atm_set_enabled(true);
#endif
    }

    void off() {
        g_arduboy_audio_enabled = false;
        ArduboyToneSoundRequest req = {.pattern = NULL};
        if(g_arduboy_sound_queue) (void)furi_message_queue_put(g_arduboy_sound_queue, &req, 0);
#ifdef ARDULIB_USE_ATM
        ardulib_atm_set_enabled(false);
        ardulib_atm_system_deinit();
#endif
        ardulib_tone_deinit();
    }

    static bool enabled() {
        return g_arduboy_audio_enabled;
    }

    void toggle() {
        if(g_arduboy_audio_enabled) {
            off();
        } else {
            on();
        }
    }

    bool systemAudioEnabled() const {
        return !furi_hal_rtc_is_flag_set(FuriHalRtcFlagStealthMode);
    }

    void saveOnOff() {
    }
};
