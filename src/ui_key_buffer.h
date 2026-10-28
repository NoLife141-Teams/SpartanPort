/* Diagnostic input bridge. Context and events are published by the SDL window
 * owner; the pad thread only reads copied atomic state, never SDL window APIs. */
#ifndef SPARTAN_UI_KEY_BUFFER_H
#define SPARTAN_UI_KEY_BUFFER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void spartanport_ui_key_context(uint32_t window, int allowed);
void spartanport_ui_key_event(uint32_t window, uint32_t scancode, int repeat);
#ifdef __cplusplus
}
#endif
#endif
