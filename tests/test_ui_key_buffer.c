/* Actual sampler and SDL event observer with fake devices/clock, no OS input. */
#include <stdint.h>
#include <time.h>
static uint64_t fixture_us=1000000;
static int fixture_clock(clockid_t kind,struct timespec *out) {
    (void)kind; out->tv_sec=(time_t)(fixture_us/1000000);
    out->tv_nsec=(long)(fixture_us%1000000)*1000; return 0;
}
#define clock_gettime fixture_clock
#define main unused_sampler_contract_main
#include "test_keyboard_sampling_contract.c"
#undef main
#undef clock_gettime
static bool fixture_focused=true;
static void event(SDL_EventType type,SDL_Scancode code,SDL_WindowID window,bool repeat) {
    if (type==SDL_EVENT_WINDOW_FOCUS_LOST) {
        spartanport_ui_key_context(7,0); return;
    }
    spartanport_ui_key_context(7,fixture_focused && !fixture_capture);
    if (type==SDL_EVENT_KEY_DOWN) spartanport_ui_key_event(window,code,repeat);
}
static PadData read_sample(void) { PadData d; sample(&d); return d; }
int main(void) {
    clear_fixture(); PadData d=read_sample(); assert(d.left_y==128);
    spartanport_ui_key_context(7,1);
    event(SDL_EVENT_KEY_DOWN,SDL_SCANCODE_S,7,false);
    assert(read_sample().left_y==128); /* Default path remains unchanged. */
    SDL_SetAtomicInt(&ui_key_buffer,1);
    event(SDL_EVENT_KEY_DOWN,SDL_SCANCODE_S,7,false);
    event(SDL_EVENT_KEY_UP,SDL_SCANCODE_S,7,false);
    assert(read_sample().left_y==255 && read_sample().left_y==255);
    fixture_us+=UI_KEY_TTL_US+1; assert(read_sample().left_y==128);
    event(SDL_EVENT_KEY_DOWN,SDL_SCANCODE_SPACE,7,false);
    assert(read_sample().buttons==BTN_CROSS && read_sample().buttons==BTN_CROSS);
    fixture_us+=UI_KEY_TTL_US+1; assert(read_sample().buttons==0);
    event(SDL_EVENT_KEY_DOWN,SDL_SCANCODE_S,7,true);
    event(SDL_EVENT_KEY_DOWN,SDL_SCANCODE_K,7,false);
    event(SDL_EVENT_KEY_DOWN,SDL_SCANCODE_S,8,false);
    assert(read_sample().left_y==128 && read_sample().buttons==0);
    event(SDL_EVENT_KEY_DOWN,SDL_SCANCODE_S,7,false);
    fixture_us+=UI_KEY_TTL_US+1;
    assert(read_sample().left_y==128);
    event(SDL_EVENT_KEY_DOWN,SDL_SCANCODE_S,7,false);
    fixture_us+=UI_KEY_TTL_US;
    assert(read_sample().left_y==255);
    ++fixture_us; assert(read_sample().left_y==128);
    event(SDL_EVENT_KEY_DOWN,SDL_SCANCODE_S,7,false);
    --fixture_us; /* Clock regression cannot make an old event fresh. */
    assert(read_sample().left_y==128); ++fixture_us;
    fixture_capture=true;
    event(SDL_EVENT_KEY_DOWN,SDL_SCANCODE_SPACE,7,false);
    sample_host(&d,NULL); assert(d.buttons==0);
    fixture_capture=false; assert(read_sample().buttons==0);
    event(SDL_EVENT_KEY_DOWN,SDL_SCANCODE_S,7,false);
    fixture_capture=true; sample_host(&d,NULL);
    fixture_capture=false; assert(read_sample().left_y==128);
    event(SDL_EVENT_KEY_DOWN,SDL_SCANCODE_S,7,false);
    event(SDL_EVENT_WINDOW_FOCUS_LOST,SDL_SCANCODE_UNKNOWN,7,false);
    spartanport_ui_key_context(7,1); /* Regaining focus must not replay the old event. */
    assert(read_sample().left_y==128);
    event(SDL_EVENT_KEY_DOWN,SDL_SCANCODE_S,7,false);
    spartanport_ui_key_context(7,0); assert(read_sample().left_y==128);
    spartanport_ui_key_context(7,1);
    assert(read_sample().left_y==128);
    fixture_focused=false;
    event(SDL_EVENT_KEY_DOWN,SDL_SCANCODE_SPACE,7,false);
    fixture_focused=true; spartanport_ui_key_context(7,1);
    assert(read_sample().buttons==0);
    assert(focus_queries==0); /* No SDL window API is called by the pad reader. */
    puts("PASS UI event buffer: opt-in only, S/Space fixed 100 ms pulse across multiple pad reads, repeat/foreign key/window rejected, expiry/clock/focus/capture cancellation; no OS input");
}
