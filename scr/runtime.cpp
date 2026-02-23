#include "../runtime.h"

#include <furi_hal.h>
#include <gui/canvas_i.h>
#include <gui/gui.h>
#include <input/input.h>

#include <stdlib.h>
#include <string.h>

#ifdef ARDULIB_USE_ATM
#include "../ATMlib.h"
#endif

extern Arduboy2Base* arduboy_ptr;
extern Sprites* sprites_ptr;
extern Sprites* ardulib_default_sprites_get(void);

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
    volatile bool screen_inverted;
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

bool arduboy_screen_inverted(void) {
    if(!rt_state) return false;
    return __atomic_load_n((bool*)&rt_state->screen_inverted, __ATOMIC_ACQUIRE);
}

void arduboy_screen_invert_toggle(void) {
    if(!rt_state) return;
    bool current = __atomic_load_n((bool*)&rt_state->screen_inverted, __ATOMIC_ACQUIRE);
    __atomic_store_n((bool*)&rt_state->screen_inverted, !current, __ATOMIC_RELEASE);
}

void arduboy_screen_invert(bool invert) {
    if(!rt_state) return;
    __atomic_store_n((bool*)&rt_state->screen_inverted, invert, __ATOMIC_RELEASE);
}

static Arduboy2Base* rt_primary_arduboy(void) {
    if(arduboy_ptr) return arduboy_ptr;
    return &rt_input_bridge;
}

static Arduboy2Base::InputContext* rt_primary_input_context(void) {
    Arduboy2Base* primary = rt_primary_arduboy();
    return primary ? primary->inputContext() : nullptr;
}

static InputKey rt_map_input_key(InputKey key) {
    return key;
}

static uint8_t rt_map_buttons(uint8_t buttons) {
    return buttons;
}

static void rt_runtime_begin(
    uint8_t* screen_buffer,
    volatile uint8_t* input_state,
    FuriMutex* game_mutex,
    volatile bool* exit_requested) {
    UNUSED(screen_buffer);
    UNUSED(input_state);
    UNUSED(game_mutex);
    UNUSED(exit_requested);

    if(!arduboy_ptr) {
        arduboy_ptr = arduboy_runtime_bridge();
    }

    if(!sprites_ptr) {
        sprites_ptr = ardulib_default_sprites_get();
    }

    if(arduboy_ptr) {
        Sprites::setArduboy(arduboy_ptr);
    }
}

static void rt_runtime_idle(void) {
    furi_delay_ms(1);
}

Arduboy2Base* arduboy_runtime_bridge(void) {
    return &rt_input_bridge;
}

uint16_t time_ms(void) {
    return (uint16_t)millis();
}

uint8_t poll_btns(void) {
    uint8_t mask = 0;

    Arduboy2Base* primary = rt_primary_arduboy();
    if(!primary) return 0;
    primary->pollButtons();

    if(primary->pressed(UP_BUTTON)) mask |= UP_BUTTON;
    if(primary->pressed(DOWN_BUTTON)) mask |= DOWN_BUTTON;
    if(primary->pressed(LEFT_BUTTON)) mask |= LEFT_BUTTON;
    if(primary->pressed(RIGHT_BUTTON)) mask |= RIGHT_BUTTON;
    if(primary->pressed(A_BUTTON)) mask |= A_BUTTON;
    if(primary->pressed(B_BUTTON)) mask |= B_BUTTON;

    return rt_map_buttons(mask);
}

static void rt_input_view_port_callback(InputEvent* event, void* context) {
    if(!event || !context) return;

    ArduboyRuntimeState* state = (ArduboyRuntimeState*)context;
    if(!__atomic_load_n((bool*)&state->input_cb_enabled, __ATOMIC_ACQUIRE)) return;
    if((event->type != InputTypePress) && (event->type != InputTypeRelease)) return;

    (void)__atomic_fetch_add((uint32_t*)&state->input_cb_inflight, 1, __ATOMIC_ACQ_REL);

    if(__atomic_load_n((bool*)&state->input_cb_enabled, __ATOMIC_ACQUIRE)) {
        InputEvent mapped_event = *event;
        mapped_event.key = rt_map_input_key(mapped_event.key);

        Arduboy2Base::InputContext* primary_ctx = rt_primary_input_context();
        if(primary_ctx) {
            Arduboy2Base::FlipperInputCallback(&mapped_event, primary_ctx);
        }
    }

    (void)__atomic_fetch_sub((uint32_t*)&state->input_cb_inflight, 1, __ATOMIC_ACQ_REL);
}

static void rt_view_port_draw_callback(Canvas* canvas, void* context) {
    ArduboyRuntimeState* state = (ArduboyRuntimeState*)context;
    if(!state || !canvas || !state->fb_mutex) return;

    if(furi_mutex_acquire(state->fb_mutex, FuriWaitForever) != FuriStatusOk) return;

    bool inverted = __atomic_load_n((bool*)&state->screen_inverted, __ATOMIC_ACQUIRE);
    uint8_t* dst = u8g2_GetBufferPtr(&canvas->fb);
    if(dst) {
        const uint8_t* src = state->front_buffer;
        if(inverted) {
            memcpy(dst, src, RuntimeBufferSize);
        } else {
            for(size_t i = 0; i < RuntimeBufferSize; i++) {
                dst[i] = (src[i] ^ 0xFFu);
            }
        }
    }

    furi_mutex_release(state->fb_mutex);
}

static bool rt_step_frame(ArduboyRuntimeState* state, uint32_t fb_wait) {
    if(!state || state->exit_requested) return false;
    if(furi_mutex_acquire(state->game_mutex, 0) != FuriStatusOk) return false;

    Arduboy2Base* primary = rt_primary_arduboy();
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
    state->screen_inverted = false;

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
    rt_runtime_begin(
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

    while(!state->exit_requested) {
        (void)rt_step_frame(state, 0);
        rt_runtime_idle();
    }

    Arduboy2Base* primary = rt_primary_arduboy();
    primary->audio.off();

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
