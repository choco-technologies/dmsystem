#ifndef DMSYSTEM_UNIT_TYPES_H
#define DMSYSTEM_UNIT_TYPES_H

#include "dmod.h"

/**
 * @file dmsystem_unit_types.h
 * @brief Plain type definitions shared by dmsystem_unit.h and dmsystem_graph.h.
 *
 * Kept separate from dmsystem_unit.h (which declares the actual DMOD module API
 * functions via dmod_dmsystem_unit_api) so that dmsystem_graph.c can include
 * just the types without re-triggering dmsystem_unit.h's registration macros: a .c
 * file's DMOD_ENABLE_REGISTRATION setting is fixed for the whole translation unit at
 * the point dmod_defs.h is first processed (its own include guard means later
 * #define/#undef has no effect) - dmsystem_graph.c needs registration ON for its own
 * dmsystem_unit_topo_sort, which would otherwise also re-register dmsystem_unit.c's
 * functions a second time in this module's link, if dmsystem_unit.h were included
 * here too.
 */

#define DMSYSTEM_MAX_UNITS               32
#define DMSYSTEM_MAX_DEPS_PER_UNIT       8
#define DMSYSTEM_MAX_ARGS               16
#define DMSYSTEM_MAX_DESC_LENGTH         64
#define DMSYSTEM_MAX_ARGS_STORAGE       160
#define DMSYSTEM_MAX_STREAM_PATH_LENGTH  DMOD_MAX_FILE_PATH_LENGTH

/**
 * @brief How a unit's process is expected to behave
 */
typedef enum
{
    DMSYSTEM_UNIT_TYPE_SIMPLE,   /*!< Long-running process: spawn and move on to the next unit */
    DMSYSTEM_UNIT_TYPE_ONESHOT,  /*!< Runs to completion before dependents may start */
} dmsystem_unit_type_t;

/**
 * @brief Restart policy applied to terminated "simple" units by the supervise loop
 */
typedef enum
{
    DMSYSTEM_RESTART_NO,        /*!< Leave it terminated */
    DMSYSTEM_RESTART_ALWAYS,    /*!< Respawn it whenever it terminates */
} dmsystem_restart_policy_t;

/**
 * @brief Current lifecycle state of a unit
 */
typedef enum
{
    DMSYSTEM_UNIT_STATE_PENDING,   /*!< Not started yet */
    DMSYSTEM_UNIT_STATE_RUNNING,   /*!< Spawned and (as far as known) still running */
    DMSYSTEM_UNIT_STATE_DONE,      /*!< Terminated/ran with a zero exit status */
    DMSYSTEM_UNIT_STATE_FAILED,    /*!< Failed to start, or terminated with a non-zero status */
    DMSYSTEM_UNIT_STATE_SKIPPED,   /*!< Never started: a required dependency failed, or it is part of a cycle */
} dmsystem_unit_state_t;

/**
 * @brief Per-unit stdin/stdout/stderr/stdlog redirection paths
 *
 * An empty string means "unset" - the stream is left at whatever default the
 * spawned process would otherwise get. Maps directly onto Dmod's
 * DMOD_STDIN/DMOD_STDOUT/DMOD_STDERR/DMOD_STDLOG stream redirection handles
 * (see dmsystem_proc.c).
 */
typedef struct
{
    char stdin_path[DMSYSTEM_MAX_STREAM_PATH_LENGTH];   /*!< Path to read stdin from */
    char stdout_path[DMSYSTEM_MAX_STREAM_PATH_LENGTH];  /*!< Path to write stdout to */
    char stderr_path[DMSYSTEM_MAX_STREAM_PATH_LENGTH];  /*!< Path to write stderr to */
    char stdlog_path[DMSYSTEM_MAX_STREAM_PATH_LENGTH];  /*!< Path to write stdlog to */
} dmsystem_streams_t;

/**
 * @brief One managed unit (the equivalent of a systemd service file)
 */
typedef struct
{
    char name[DMOD_MAX_MODULE_NAME_LENGTH];         /*!< Unit name (the INI section name) */
    char description[DMSYSTEM_MAX_DESC_LENGTH];     /*!< Human readable description, for logging */
    char exec[DMOD_MAX_MODULE_NAME_LENGTH];          /*!< Module name to run/spawn */

    char* argv[DMSYSTEM_MAX_ARGS];                  /*!< argv[0] == exec, NULL-terminated */
    int   argc;
    char  args_storage[DMSYSTEM_MAX_ARGS_STORAGE];  /*!< Backing storage sliced up by argv[1..] */

    dmsystem_unit_type_t      type;
    dmsystem_restart_policy_t restart;
    dmsystem_streams_t        streams;               /*!< stdin/stdout/stderr/stdlog redirections */

    char   after[DMSYSTEM_MAX_DEPS_PER_UNIT][DMOD_MAX_MODULE_NAME_LENGTH];     /*!< Ordering-only dependencies */
    size_t after_count;

    char   requires[DMSYSTEM_MAX_DEPS_PER_UNIT][DMOD_MAX_MODULE_NAME_LENGTH]; /*!< Ordering + hard dependencies */
    size_t requires_count;

    dmsystem_unit_state_t state;
    Dmod_Pid_t             pid;          /*!< Valid while state == RUNNING (simple units only) */
    int                    exit_status;
} dmsystem_unit_t;

/**
 * @brief Fixed-capacity collection of units, in the order they were declared in the config
 */
typedef struct
{
    dmsystem_unit_t units[DMSYSTEM_MAX_UNITS];
    size_t          count;
} dmsystem_unit_list_t;

#endif /* DMSYSTEM_UNIT_TYPES_H */
