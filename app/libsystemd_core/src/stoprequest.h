#ifndef LIBSYSTEMD_STOPREQUEST_H
#define LIBSYSTEMD_STOPREQUEST_H

#include <stdbool.h>
#include "libsystemd.h"

/**
 * @file stoprequest.h
 * @brief Private interface of the stop request registry (see stoprequest.c)
 *
 * Maps a process ID to the wakeup semaphore that process registered for
 * itself with libsystemd_set_stop_semaphore(), plus its "stop requested"
 * flag. Used by libsystemd_stop_service_internal() (serviceapi.c) to ask a
 * service to leave on its own before falling back to dmosi_process_kill().
 */

/**
 * @brief Allocate the registry (called from libsystemd_serviceapi_init())
 *
 * @retval 0       Ready (or already initialized).
 * @retval -ENOMEM Allocation failed.
 */
int libsystemd_stoprequest_init(void);

/**
 * @brief Release the registry (called from libsystemd_serviceapi_deinit())
 *
 * Unregisters the process exit callbacks still attached to live processes, so
 * no callback into this module outlives it.
 */
void libsystemd_stoprequest_deinit(void);

/**
 * @brief Ask a process to stop, if it registered for stop requests
 *
 * Sets the process's "stop requested" flag and posts its wakeup semaphore.
 * Runs no code of the service and never blocks beyond the registry lock,
 * which is itself only ever held for a few non-blocking operations.
 *
 * @param pid Process to ask.
 *
 * @retval true  @p pid is registered and has been asked.
 * @retval false @p pid is not registered (nothing was done).
 */
bool libsystemd_stoprequest_send(dmosi_process_id_t pid);

#endif // LIBSYSTEMD_STOPREQUEST_H
