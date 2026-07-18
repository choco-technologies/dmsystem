#ifndef SYSTEMD_TYPES_H
#define SYSTEMD_TYPES_H

#include <stdint.h>
#include "dmosi.h"

/**
 * @brief type for storing service's context
 */
typedef struct systemd_service* systemd_service_t;

/**
 * @brief stores status of the given service
 */
typedef struct 
{
    dmosi_process_state_t state;        //!< state of the service's process
    dmosi_process_id_t    pid;          //!< PID of the process related to the service
} systemd_service_status_t;

#endif // SYSTEMD_TYPES_H