/* Experimental passive diagnostics. No game commands or liveness decision. */
#ifndef SPARTAN_HEALTH_PASSIVE_BRIDGE_H
#define SPARTAN_HEALTH_PASSIVE_BRIDGE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Start/tick/stop belong exclusively to the existing SDL window thread. */
int spartanport_health_passive_start(void);
void spartanport_health_passive_tick(void);
void spartanport_health_passive_stop(void);
/* Loader-only construction, before guest module initialization. */
unsigned spartanport_health_passive_build(unsigned char *out, size_t capacity,
                                        uintptr_t slots, uintptr_t resume);
/* Loader reports missing/unsafe hook without touching the consumer's file. */
void spartanport_health_passive_unavailable(unsigned reason);
#ifdef __cplusplus
}
#endif
#endif
