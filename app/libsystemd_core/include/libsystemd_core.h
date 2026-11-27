#ifndef LIBSYSTEMD_CORE_H
#define LIBSYSTEMD_CORE_H

#include <stdbool.h>
#include "dmod.h"
#include "libsystemd_core_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tell whether the implementation of libsystemd (this module) is loaded and takes the calls of the front
 *
 * The API of libsystemd.h is offered by the module libsystemd, which passes the calls on to this module once it is loaded.
 * A module that needs the units - a service manager, a shell command, a test - calls this to require this module (the
 * dependency of a module is made of the API it calls) and to check that the calls will be served, rather than answered
 * with -ENODEV.
 *
 * @return true when the calls of libsystemd.h are passed on to the units
 */
dmod_libsystemd_core_api(1.0, bool, _is_ready, (void));

#ifdef __cplusplus
}
#endif

#endif // LIBSYSTEMD_CORE_H
