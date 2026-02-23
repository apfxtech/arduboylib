#include "../runtime.h"

#include <furi_hal.h>
#include <gui/canvas_i.h>
#include <gui/gui.h>
#include <input/input.h>

#include <stdlib.h>
#include <string.h>

namespace {

static constexpr size_t RuntimeWidth = 128u;
static constexpr size_t RuntimeHeight = 64u;
static constexpr size_t RuntimeBufferSize = (RuntimeWidth * RuntimeHeight) / 8u;

typedef struct {
    uint8_t screen_buffer[RuntimeBufferSize];
    uint8_t front_buffer[RuntimeBufferSize];

    Gui* gui;
    ViewPort* view_port;
    FuriMutex* fb_mutex;
    FuriMutex* game_mutex;

    volatile uint8_t input_state;
    volatile bool exit_requested;
    volatile bool input_cb_enabled;
    volatile uint32_t input_cb_inflight;
} ArduboyRuntimeState;

static ArduboyRuntimeState* rt_state = NULL;
static Arduboy2Base rt_input_bridge;

static void rt_wait_input_callbacks_idle(ArduboyRuntimeState* state) {
    if(!state) return;
    while(__atomic_load_n((uint32_t*)&state->input_cb_inflight, __ATOMIC_ACQUIRE) != 0) {
        furi_delay_ms(1);
    }
}

} // namespace

FuriMessageQueue* g_arduboy_sound_queue = NULL;
FuriThread* g_arduboy_sound_thread = NULL;
volatile bool g_arduboy_sound_thread_running = false;
volatile bool g_arduboy_audio_enabled = false;
volatile bool g_arduboy_tones_playing = false;
volatile uint8_t g_arduboy_volume_mode = VOLUME_IN_TONE;
volatile bool g_arduboy_force_high = false;
volatile bool g_arduboy_force_norm = false;

uint8_t* buf = NULL;

void __attribute__((weak)) arduboy_runtime_on_begin(
    uint8_t* screen_buffer,
    volatile uint8_t* input_state,
    FuriMutex* game_mutex,
    volatile bool* exit_requested) {
    UNUSED(screen_buffer);
    UNUSED(input_state);
    UNUSED(game_mutex);
    UNUSED(exit_requested);
}

Arduboy2Base* __attribute__((weak)) arduboy_runtime_primary_arduboy(void) {
    return nullptr;
}

Arduboy2Base::InputContext* __attribute__((weak)) arduboy_runtime_primary_input_context(void) {
    Arduboy2Base* primary = arduboy_runtime_primary_arduboy();
    return primary ? primary->inputContext() : nullptr;
}

InputKey __attribute__((weak)) arduboy_runtime_map_input_key(InputKey key) {
    return key;
}

uint8_t __attribute__((weak)) arduboy_runtime_map_buttons(uint8_t buttons) {
    return buttons;
}

uint8_t __attribute__((weak)) arduboy_runtime_transform_byte(uint8_t value, size_t index) {
    UNUSED(index);
    return (uint8_t)(value ^ 0xFFu);
}

uint32_t __attribute__((weak)) arduboy_runtime_fps(void) {
    return 60u;
}

void __attribute__((weak)) arduboy_runtime_idle(void) {
}

void __attribute__((weak)) arduboy_runtime_on_stop(void) {
}

Arduboy2Base* arduboy_runtime_bridge(void) {
    return &rt_input_bridge;
}

uint16_t time_ms(void) {
    return (uint16_t)millis();
}

uint8_t poll_btns(void) {
    uint8_t mask = 0;

    rt_input_bridge.pollButtons();

    if(rt_input_bridge.pressed(UP_BUTTON)) mask |= UP_BUTTON;
    if(rt_input_bridge.pressed(DOWN_BUTTON)) mask |= DOWN_BUTTON;
    if(rt_input_bridge.pressed(LEFT_BUTTON)) mask |= LEFT_BUTTON;
    if(rt_input_bridge.pressed(RIGHT_BUTTON)) mask |= RIGHT_BUTTON;
    if(rt_input_bridge.pressed(A_BUTTON)) mask |= A_BUTTON;
    if(rt_input_bridge.pressed(B_BUTTON)) mask |= B_BUTTON;

    return arduboy_runtime_map_buttons(mask);
}

static void rt_input_view_port_callback(InputEvent* event, void* context) {
    if(!event || !context) return;

    ArduboyRuntimeState* state = (ArduboyRuntimeState*)context;
    if(!__atomic_load_n((bool*)&state->input_cb_enabled, __ATOMIC_ACQUIRE)) return;
    if((event->type != InputTypePress) && (event->type != InputTypeRelease)) return;

    (void)__atomic_fetch_add((uint32_t*)&state->input_cb_inflight, 1, __ATOMIC_ACQ_REL);

    if(__atomic_load_n((bool*)&state->input_cb_enabled, __ATOMIC_ACQUIRE)) {
        InputEvent mapped_event = *event;
        mapped_event.key = arduboy_runtime_map_input_key(mapped_event.key);

        Arduboy2Base::FlipperInputCallback(&mapped_event, rt_input_bridge.inputContext());

        Arduboy2Base::InputContext* primary_ctx = arduboy_runtime_primary_input_context();
        if(primary_ctx && (primary_ctx != rt_input_bridge.inputContext())) {
            Arduboy2Base::FlipperInputCallback(&mapped_event, primary_ctx);
        }
    }

    (void)__atomic_fetch_sub((uint32_t*)&state->input_cb_inflight, 1, __ATOMIC_ACQ_REL);
}

static void rt_view_port_draw_callback(Canvas* canvas, void* context) {
    ArduboyRuntimeState* state = (ArduboyRuntimeState*)context;
    if(!state || !canvas || !state->fb_mutex) return;

    if(furi_mutex_acquire(state->fb_mutex, FuriWaitForever) != FuriStatusOk) return;

    uint8_t* dst = canvas_get_buffer(canvas);
    if(dst) {
        const uint8_t* src = state->front_buffer;
        for(size_t i = 0; i < RuntimeBufferSize; i++) {
            dst[i] = arduboy_runtime_transform_byte(src[i], i);
        }
    }

    furi_mutex_release(state->fb_mutex);
}

static bool rt_step_frame(ArduboyRuntimeState* state, uint32_t fb_wait) {
    if(!state || state->exit_requested) return false;
    if(furi_mutex_acquire(state->game_mutex, 0) != FuriStatusOk) return false;

    Arduboy2Base* primary = arduboy_runtime_primary_arduboy();
    uint32_t frame_before = primary ? primary->frameCount() : 0u;

    loop();

    uint32_t frame_after = primary ? primary->frameCount() : 1u;
    bool has_new_frame = primary ? (frame_after != frame_before) : true;

    if(has_new_frame) {
        if(furi_mutex_acquire(state->fb_mutex, fb_wait) == FuriStatusOk) {
            memcpy(state->front_buffer, state->screen_buffer, RuntimeBufferSize);
            furi_mutex_release(state->fb_mutex);
        }

        if(primary) {
            primary->applyDeferredDisplayOps();
        }

        if(state->view_port) view_port_update(state->view_port);
    }

    furi_mutex_release(state->game_mutex);
    return has_new_frame;
}

extern "C" int32_t arduboy_app(void* p) {
    UNUSED(p);

    rt_state = (ArduboyRuntimeState*)malloc(sizeof(ArduboyRuntimeState));
    if(!rt_state) return -1;
    memset(rt_state, 0, sizeof(ArduboyRuntimeState));

    ArduboyRuntimeState* state = rt_state;

    state->fb_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    state->game_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    state->input_cb_enabled = true;
    state->input_cb_inflight = 0;

    if(!state->fb_mutex || !state->game_mutex) {
        if(state->fb_mutex) furi_mutex_free(state->fb_mutex);
        if(state->game_mutex) furi_mutex_free(state->game_mutex);
        free(state);
        rt_state = NULL;
        return -1;
    }

    memset(state->screen_buffer, 0x00, RuntimeBufferSize);
    memset(state->front_buffer, 0x00, RuntimeBufferSize);
    buf = state->screen_buffer;

    rt_input_bridge.begin(
        state->screen_buffer, &state->input_state, state->game_mutex, &state->exit_requested);
    arduboy_runtime_on_begin(
        state->screen_buffer, &state->input_state, state->game_mutex, &state->exit_requested);

    state->gui = (Gui*)furi_record_open(RECORD_GUI);
    state->view_port = view_port_alloc();
    if(!state->gui || !state->view_port) {
        if(state->view_port) view_port_free(state->view_port);
        if(state->gui) furi_record_close(RECORD_GUI);
        furi_mutex_free(state->fb_mutex);
        furi_mutex_free(state->game_mutex);
        free(state);
        rt_state = NULL;
        buf = NULL;
        return -1;
    }

    view_port_draw_callback_set(state->view_port, rt_view_port_draw_callback, state);
    view_port_input_callback_set(state->view_port, rt_input_view_port_callback, state);
    gui_add_view_port(state->gui, state->view_port, GuiLayerFullscreen);

    if(furi_mutex_acquire(state->game_mutex, FuriWaitForever) == FuriStatusOk) {
        setup();
        furi_mutex_release(state->game_mutex);
    }

    if(furi_mutex_acquire(state->fb_mutex, FuriWaitForever) == FuriStatusOk) {
        memcpy(state->front_buffer, state->screen_buffer, RuntimeBufferSize);
        furi_mutex_release(state->fb_mutex);
    }

    view_port_update(state->view_port);

    const uint32_t runtime_fps = arduboy_runtime_fps();
    const uint32_t tick_hz = furi_kernel_get_tick_frequency();
    uint32_t frame_ticks = 0;
    uint32_t next_tick = furi_get_tick();
    if(runtime_fps && tick_hz) {
        frame_ticks = (tick_hz + (runtime_fps / 2u)) / runtime_fps;
        if(frame_ticks == 0) frame_ticks = 1;
    }

    while(!state->exit_requested) {
        if(frame_ticks) {
            const uint32_t now = furi_get_tick();

            if((int32_t)(now - next_tick) < 0) {
                const uint32_t dt_ticks = next_tick - now;
                const uint32_t dt_ms = (dt_ticks * 1000u) / tick_hz;
                furi_delay_ms(dt_ms ? dt_ms : 1);
                arduboy_runtime_idle();
                continue;
            }

            if((int32_t)(now - next_tick) > (int32_t)(frame_ticks * 2u)) {
                next_tick = now;
            }
            next_tick += frame_ticks;
        }

        (void)rt_step_frame(state, 0);
        arduboy_runtime_idle();
    }

    arduboy_runtime_on_stop();

    __atomic_store_n((bool*)&state->input_cb_enabled, false, __ATOMIC_RELEASE);

    if(state->view_port && state->gui) {
        view_port_enabled_set(state->view_port, false);
        gui_remove_view_port(state->gui, state->view_port);
        view_port_free(state->view_port);
        state->view_port = NULL;
    }

    rt_wait_input_callbacks_idle(state);

    if(state->gui) {
        furi_record_close(RECORD_GUI);
        state->gui = NULL;
    }

    if(state->fb_mutex) {
        furi_mutex_free(state->fb_mutex);
        state->fb_mutex = NULL;
    }

    if(state->game_mutex) {
        furi_mutex_free(state->game_mutex);
        state->game_mutex = NULL;
    }

    free(state);
    rt_state = NULL;
    buf = NULL;

    return 0;
}
