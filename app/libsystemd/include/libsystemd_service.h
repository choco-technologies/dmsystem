#ifndef LIBSYSTEMD_SERVICE_H
#define LIBSYSTEMD_SERVICE_H
#include "dmod.h"
#include "libsystemd_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optional paired lifecycle DIF for shared Library modules. dmod_init/deinit
 * own module lifetime; these callbacks own each unit instance. argc/argv are
 * the fully expanded exec/args values (argv[0] is exec). Strings are borrowed
 * for the duration of the call. START failure must leave that unit inactive;
 * STOP failure leaves it active and may be retried. No process is spawned.
 * Modules without either callback retain the legacy enable/disable behavior.
 * A callback-backed stop does not unload shared code or disconnect consumers. */
dmod_libsystemd_dif(1.0, int, _service_start,
    (const char* unit_name, int argc, const char* const argv[]));
dmod_libsystemd_dif(1.0, int, _service_stop, (const char* unit_name));
#ifdef __cplusplus
}
#endif
#endif
