/* Offline characterization of the actual pad sampler. Fake device states only:
 * no SDL window, OS input, controller enumeration, game, save or command file. */
#define _GNU_SOURCE
#include <assert.h>
#include <SDL3/SDL.h>

/* The upstream switch-attack path is disabled in this sampler fixture. */
int runtime_memory_is_mapped(uintptr_t address, uint64_t size) {
    (void)address; (void)size; assert(!"switch attacks must remain disabled"); return 0;
}

static bool fixture_keys[SDL_SCANCODE_COUNT];
static bool fixture_pad_buttons[SDL_GAMEPAD_BUTTON_COUNT];
static Sint16 fixture_pad_axes[SDL_GAMEPAD_AXIS_COUNT];
static bool fixture_video = true;
static bool fixture_capture;
static unsigned keyboard_reads, focus_queries;
static int fixture_pad_token;

static SDL_InitFlags fixture_was_init(SDL_InitFlags flags) {
    return flags & (SDL_INIT_GAMEPAD | (fixture_video ? SDL_INIT_VIDEO : 0));
}
static const bool *fixture_keyboard(int *count) {
    ++keyboard_reads;
    if (count) *count=SDL_SCANCODE_COUNT;
    return fixture_keys;
}
static SDL_Window *fixture_focus(void) { ++focus_queries; return NULL; }
static SDL_JoystickID *fixture_gamepads(int *count) { *count=0; return NULL; }
static bool fixture_connected(SDL_Gamepad *pad) { (void)pad; return true; }
static bool fixture_button(SDL_Gamepad *pad, SDL_GamepadButton button) {
    (void)pad; return fixture_pad_buttons[button];
}
static Sint16 fixture_axis(SDL_Gamepad *pad, SDL_GamepadAxis axis) {
    (void)pad; return fixture_pad_axes[axis];
}
static int fixture_touchpads(SDL_Gamepad *pad) { (void)pad; return 0; }
static SDL_MouseButtonFlags fixture_mouse(float *x, float *y) {
    if (x) *x=0; if (y) *y=0; return 0;
}
static bool fixture_watch(SDL_EventFilter filter, void *userdata) {
    (void)filter; (void)userdata; return true;
}

#define SDL_WasInit fixture_was_init
#define SDL_GetKeyboardState fixture_keyboard
#define SDL_GetKeyboardFocus fixture_focus
#define SDL_GetGamepads fixture_gamepads
#define SDL_GamepadConnected fixture_connected
#define SDL_GetGamepadButton fixture_button
#define SDL_GetGamepadAxis fixture_axis
#define SDL_GetNumGamepadTouchpads fixture_touchpads
#define SDL_GetMouseState fixture_mouse
#define SDL_AddEventWatch fixture_watch
#include "../src/runtime_pad.c"

int spartanportgpu_overlay_captures_input(void) { return fixture_capture; }
int spartanportgpu_vibration_percent(void) { return 0; }
void spartanportgpu_engine_fps_cycle(void) {}
int spartanportgpu_mouse_prompt_distance(void) { return 48; }
void spartanportgpu_mouse_pause_key(void) {}
int spartanportgpu_mouse_aim_style(void) { return 0; }
int spartanportgpu_mouse_aim_sensitivity(void) { return 100; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name; return 0;
}

static void clear_fixture(void) {
    memset(fixture_keys,0,sizeof(fixture_keys));
    memset(fixture_pad_buttons,0,sizeof(fixture_pad_buttons));
    memset(fixture_pad_axes,0,sizeof(fixture_pad_axes));
    fixture_capture=false; fixture_video=true; gamepad=NULL; sdl_ready=1;
}
static void pass(const char *label) { printf("PASS %s\n",label); }

int main(void) {
    (void)&fixture_focus; /* Kept as an interposer if the sampler gains a focus query. */
#ifdef SPARTAN_KEY_TRACE_RUNTIME
    const int trace_started=spartanport_key_trace_start();
    if(trace_started) spartanport_key_trace_window(7,KT_FOCUS,1);
#endif
    const char *unset[]={"SPARTAN_KEY_MAP","SPARTAN_PAD_MAP","SPARTAN_STICK_PROMPTS",
        "SPARTAN_PAD_FILE","SPARTAN_PAD_REPLAY","SPARTAN_PAD_RECORD"};
    for (size_t i=0;i<sizeof(unset)/sizeof(*unset);++i) assert(!getenv(unset[i]));
    PadData d;
    clear_fixture();fixture_keys[SDL_SCANCODE_S]=true;sample(&d);
    assert(d.left_y==255 && d.buttons==0);
    fixture_keys[SDL_SCANCODE_S]=false;sample(&d);assert(d.left_y==128);
    pass("S sets left Y to 255; release restores neutral without extending input");
    fixture_keys[SDL_SCANCODE_S]=true;fixture_keys[SDL_SCANCODE_S]=false;sample(&d);
    assert(d.left_y==128);pass("A complete S pulse before the next read leaves left Y neutral");
    clear_fixture();gamepad=(SDL_Gamepad *)&fixture_pad_token;fixture_pad_axes[SDL_GAMEPAD_AXIS_LEFTY]=12000;
    sample(&d);assert(d.left_y==axis(12000));fixture_keys[SDL_SCANCODE_S]=true;
    sample(&d);assert(d.left_y==255);pass("S overrides gamepad Y only while held");
    clear_fixture();fixture_keys[SDL_SCANCODE_K]=true;fixture_keys[SDL_SCANCODE_SPACE]=true;
    sample(&d);assert(d.left_y==128 && d.buttons==(BTN_DOWN|BTN_CROSS));
    pass("Unrelated K and Space do not imply S or left-stick movement");
    clear_fixture(); fixture_keys[SDL_SCANCODE_K]=true; sample_host(&d,NULL);
    assert(d.buttons==BTN_DOWN && d.left_y==128 && d.right_y==128);
    pass("K maps to d-pad Down, not a stick");
    clear_fixture(); fixture_keys[SDL_SCANCODE_DOWN]=true; sample_host(&d,NULL);
    assert(d.buttons==0 && d.right_y==255);
    pass("Down arrow is right-stick down, not d-pad Down");
    clear_fixture(); fixture_keys[SDL_SCANCODE_SPACE]=true; sample_host(&d,NULL);
    assert(d.buttons==BTN_CROSS);
    clear_fixture(); fixture_keys[SDL_SCANCODE_RETURN]=true; sample_host(&d,NULL);
    assert(d.buttons==BTN_OPTIONS);
    pass("Space is Cross; Return is Options");

    clear_fixture(); gamepad=(SDL_Gamepad *)&fixture_pad_token;
    fixture_keys[SDL_SCANCODE_K]=true; sample_host(&d,NULL);
    assert(d.buttons==BTN_DOWN);
    fixture_pad_buttons[SDL_GAMEPAD_BUTTON_SOUTH]=true; sample_host(&d,NULL);
    assert(d.buttons==(BTN_DOWN|BTN_CROSS));
    fixture_pad_buttons[SDL_GAMEPAD_BUTTON_DPAD_UP]=true; sample_host(&d,NULL);
    assert(d.buttons==(BTN_DOWN|BTN_CROSS|BTN_UP));
    pass("Gamepad does not disable keyboard; buttons including opposite directions are ORed");

    clear_fixture(); gamepad=(SDL_Gamepad *)&fixture_pad_token;
    fixture_pad_axes[SDL_GAMEPAD_AXIS_LEFTY]=12000; sample_host(&d,NULL);
    assert(d.left_y==axis(12000));
    fixture_keys[SDL_SCANCODE_W]=true; sample_host(&d,NULL); assert(d.left_y==0);
    fixture_keys[SDL_SCANCODE_W]=false; sample_host(&d,NULL); assert(d.left_y==axis(12000));
    pass("Unheld keyboard axes preserve gamepad state; held direction overrides only that axis");

    clear_fixture(); fixture_keys[SDL_SCANCODE_K]=true; fixture_capture=true;
    const unsigned before=keyboard_reads; sample_host(&d,NULL);
    assert(d.buttons==0 && keyboard_reads==before);
    pass("Overlay capture neutralizes input before reading keyboard");
    sample(&d); assert(hold_after_capture && d.buttons==0);
    fixture_capture=false; sample(&d); assert(d.buttons==0 && hold_after_capture);
    fixture_keys[SDL_SCANCODE_K]=false;
    gamepad=(SDL_Gamepad *)&fixture_pad_token;
    fixture_pad_buttons[SDL_GAMEPAD_BUTTON_SOUTH]=true;
    sample(&d); assert(d.buttons==0 && hold_after_capture);
    fixture_pad_buttons[SDL_GAMEPAD_BUTTON_SOUTH]=false;
    sample(&d); assert(!hold_after_capture && d.buttons==0);
    fixture_keys[SDL_SCANCODE_K]=true; sample(&d); assert(d.buttons==BTN_DOWN);
    pass("After overlay, held keyboard or controller buttons require a neutral read before rearming");

    clear_fixture(); assert(pad_init()==0); assert(pad_open(1,0,0,NULL)==PAD_HANDLE);
    assert(pad_read_state(PAD_HANDLE,&d)==0 && d.buttons==0);
    fixture_keys[SDL_SCANCODE_K]=true; /* press occurs between guest reads */
    fixture_keys[SDL_SCANCODE_K]=false; /* release also occurs before next read */
    assert(pad_read_state(PAD_HANDLE,&d)==0 && d.buttons==0);
    pass("A complete key pulse between guest reads is lost by the actual sampler");
    fixture_keys[SDL_SCANCODE_K]=true;
    assert(pad_read_state(PAD_HANDLE,&d)==0 && d.buttons==BTN_DOWN);
    fixture_keys[SDL_SCANCODE_K]=false;
    assert(pad_read_state(PAD_HANDLE,&d)==0 && d.buttons==0);
    pass("Pulse spanning a guest read is visible; release has no keyboard hold extension");
    PadData buffered[4]; memset(buffered,0xa5,sizeof(buffered));
    fixture_keys[SDL_SCANCODE_K]=true;
    assert(pad_read(PAD_HANDLE,buffered,4)==1 && buffered[0].buttons==BTN_DOWN);
    assert(buffered[1].buttons==0xa5a5a5a5u);
    pass("Buffered pad API returns one current sample, not an event history");

    clear_fixture(); fixture_video=false; fixture_keys[SDL_SCANCODE_K]=true;
    const unsigned before_no_video=keyboard_reads; sample_host(&d,NULL);
    assert(d.buttons==0 && keyboard_reads==before_no_video);
    pass("Without SDL video initialized, keyboard state is not sampled");
    fixture_video=true; sample_host(&d,NULL);
    assert(d.buttons==BTN_DOWN && focus_queries==0);
    fixture_keys[SDL_SCANCODE_K]=false; sample_host(&d,NULL); assert(d.buttons==0);
    pass("Sampler has no explicit SDL focus query; it trusts the SDL key-state array");
    puts("16 scenarios passed; fake device states, no host input, no SDL window, no game");
#ifdef SPARTAN_KEY_TRACE_RUNTIME
    if(trace_started) spartanport_key_trace_stop();
#endif
    return 0;
}
