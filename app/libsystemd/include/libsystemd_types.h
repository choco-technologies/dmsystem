#ifndef LIBSYSTEMD_TYPES_H
#define LIBSYSTEMD_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "dmosi.h"

/**
 * @brief type for storing service's context
 */
typedef struct libsystemd_service* libsystemd_service_t;

/**
 * @brief stores status of the given service
 */
typedef struct
{
    dmosi_process_state_t state;        //!< state of the service's process
    dmosi_process_id_t    pid;          //!< PID of the process related to the service
} libsystemd_service_status_t;

typedef struct
{
    const char*                     unit_name;
    libsystemd_service_status_t     status;
} libsystemd_service_info_t;

/**
 * @brief pointer to a visitor function
 */
typedef bool (*libsystemd_visitor_t)(const libsystemd_service_info_t* info, void* user_ptr);

/**
 * @brief list of services
 */
typedef struct libsystemd_services* libsystemd_services_t;

#endif // LIBSYSTEMD_TYPES_H
