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

/**
 * @brief Startup behavior selected by a unit's "type" ini key
 */
typedef enum
{
    LIBSYSTEMD_SERVICE_TYPE_SIMPLE,   //!< Default. The process is expected to keep running until stopped; an unexpected exit is logged as a warning.
    LIBSYSTEMD_SERVICE_TYPE_ONESHOT,  //!< The process is expected to run to completion and exit on its own; a clean (status 0) exit is not treated as a crash.
} libsystemd_service_type_t;

/**
 * @brief Restart policy selected by a unit's "restart" ini key
 */
typedef enum
{
    LIBSYSTEMD_RESTART_NO,          //!< Default. Never automatically restart the unit when its process exits on its own.
    LIBSYSTEMD_RESTART_ALWAYS,      //!< Restart the unit whenever its process exits on its own, regardless of exit status.
    LIBSYSTEMD_RESTART_ON_FAILURE,  //!< Restart the unit only when its process exits on its own with a non-zero status.
} libsystemd_restart_policy_t;

typedef struct
{
    const char*                     unit_name;
    const char*                     description;     //!< From the "description" ini key, or NULL if unset.
    libsystemd_service_type_t       type;             //!< From the "type" ini key ("simple"/"oneshot"), defaults to ::LIBSYSTEMD_SERVICE_TYPE_SIMPLE.
    libsystemd_restart_policy_t     restart_policy;   //!< From the "restart" ini key ("no"/"always"/"on-failure"), defaults to ::LIBSYSTEMD_RESTART_NO.
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
