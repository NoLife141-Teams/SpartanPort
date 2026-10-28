/* Passive diagnostic only: S and left-stick Y, never text or other key IDs. */
#ifndef SPARTAN_KEYBOARD_TRACE_BRIDGE_H
#define SPARTAN_KEYBOARD_TRACE_BRIDGE_H
#include <stdint.h>
enum { KT_FOCUS=1, KT_CAPTURE=2, KT_TEXT=4 };
enum { KT_S_VALID=1, KT_S_BEFORE=2, KT_S_AFTER=4, KT_HOST_Y_VALID=8,
       KT_FINAL_Y_VALID=16, KT_HOST_BLOCK=32, KT_GATE_BLOCK=64,
       KT_HOLD_BEFORE=128, KT_HOLD_AFTER=256, KT_PAD_PRESENT=512,
       KT_PAD_Y_VALID=1024 };
typedef struct SpartanKeyTraceRead {
    uint64_t id, begin, context;
    uint32_t flags;
    uint8_t pad_y, host_y, final_y;
} SpartanKeyTraceRead;
#ifdef __cplusplus
extern "C" {
#endif
#ifdef SPARTAN_KEY_TRACE_DISABLED_STUBS
static inline SpartanKeyTraceRead spartanport_key_trace_begin(void) { SpartanKeyTraceRead r={0}; return r; }
static inline void spartanport_key_trace_end(SpartanKeyTraceRead r) { (void)r; }
#else
/* Owner calls, only on the SDL window thread; start precedes guest threads. */
int spartanport_key_trace_start(void);
int spartanport_key_trace_enabled(void);
void spartanport_key_trace_window(uint32_t window, uint32_t context_flags, int new_pump);
void spartanport_key_trace_s(uint32_t window, uint32_t context_flags, int down, int repeat);
void spartanport_key_trace_tick(void);
void spartanport_key_trace_stop(void);
/* Pad read token explicitly travels with that read; no last-event association. */
SpartanKeyTraceRead spartanport_key_trace_begin(void);
void spartanport_key_trace_end(SpartanKeyTraceRead read);
#endif
#ifdef __cplusplus
}
#endif
#endif
