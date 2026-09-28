#ifndef LIBSYSTEMD_STOPHANDLER_H
#define LIBSYSTEMD_STOPHANDLER_H

#include <stdbool.h>
#include "libsystemd.h"

/**
 * @file stophandler.h
 * @brief Private interface of the stop handler registry (see stophandler.c)
 *
 * Maps a process ID to the libsystemd_stop_handler_t that process registered
 * for itself with libsystemd_set_stop_handler(). Used by
 * libsystemd_stop_service_internal() (serviceapi.c) to ask a service to leave
 * on its own before falling back to dmosi_process_kill().
 */

/**
 * @brief Allocate the registry (called from libsystemd_serviceapi_init())
 *
 * @retval 0       Ready (or already initialized).
 * @retval -ENOMEM Allocation failed.
 */
int libsystemd_stophandler_init(void);

/**
 * @brief Release the registry (called from libsystemd_serviceapi_deinit())
 *
 * Unregisters the process exit callbacks still attached to live processes, so
 * no callback into this module outlives it.
 */
void libsystemd_stophandler_deinit(void);

/**
 * @brief Ask a process to stop through the handler it registered, if any
 *
 * The handler runs in the caller's context with the registry locked, so a
 * concurrent libsystemd_set_stop_handler() (e.g. the service unregistering
 * before it frees the handler's argument) waits until the call returns.
 *
 * @param pid Process to ask.
 *
 * @retval true  A handler was registered for @p pid and has been called.
 * @retval false No handler is registered for @p pid (nothing was called).
 */
bool libsystemd_stophandler_request(dmosi_process_id_t pid);

#endif // LIBSYSTEMD_STOPHANDLER_H
