#ifndef DMSYSTEM_PROC_H
#define DMSYSTEM_PROC_H

#include "dmsystem_unit.h"

/**
 * @file dmsystem_proc.h
 * @brief Thin wrapper around Dmod_SpawnModule and dmosi's process API.
 *
 * Plain internal header (see dmsystem_config.h) - this is the only place in
 * dmsystem_core that calls into dmosi/Dmod's process-management builtin API,
 * isolating it here keeps the orchestration logic in dmsystem_core.c free of any
 * direct dependency on a live dmosi backend being connected.
 */

/**
 * @brief Starts one unit according to its type
 *
 * Both cases spawn via Dmod_SpawnModule (honoring unit->streams'
 * stdin/stdout/stderr/stdlog redirections, if any) rather than Dmod_RunModule,
 * which does not accept stream redirections at all:
 * - DMSYSTEM_UNIT_TYPE_ONESHOT: blocks until the spawned process exits;
 *   unit->exit_status and unit->state (DONE/FAILED) are set before return.
 * - DMSYSTEM_UNIT_TYPE_SIMPLE: returns immediately once spawned; unit->pid and
 *   unit->state (RUNNING/FAILED) are set before return.
 *
 * @return true if the unit started/completed successfully (dependents may proceed),
 *         false if it failed to start or exited with a non-zero status
 */
bool dmsystem_proc_start(dmsystem_unit_t* unit);

/**
 * @brief Polls a RUNNING "simple" unit for termination
 *
 * If the unit has terminated, records its exit status, updates its state to
 * DONE/FAILED, releases the underlying process handle, and returns true - the
 * caller should then decide whether to restart it based on unit->restart.
 *
 * @return true if the unit just transitioned out of RUNNING, false if it is
 *         still running or not applicable (wrong type/state)
 */
bool dmsystem_proc_poll(dmsystem_unit_t* unit);

#endif /* DMSYSTEM_PROC_H */
