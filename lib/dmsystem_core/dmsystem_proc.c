#include "dmsystem_proc.h"
#include "dmod.h"
#include "dmosi.h"
#include <errno.h>

/**
 * @brief Builds the stream-redirection list for @p unit into @p out_entries
 *
 * @param out_entries Backing storage for the returned Streams.Entries; must stay
 *                     alive for as long as the returned value is used
 * @return A Dmod_StreamRedirections_t referencing @p out_entries. Count == 0 if the
 *         unit has none of stdin/stdout/stderr/stdlog configured - callers should
 *         pass NULL to Dmod_SpawnModule in that case rather than this empty struct,
 *         matching the "no redirection" convention used elsewhere in DMOD.
 */
static Dmod_StreamRedirections_t build_streams(const dmsystem_unit_t* unit, Dmod_StreamRedirection_t out_entries[4])
{
    size_t count = 0;

    if (unit->streams.stdin_path[0] != '\0')
        out_entries[count++] = (Dmod_StreamRedirection_t){ .StdHandle = DMOD_STDIN, .Path = unit->streams.stdin_path };
    if (unit->streams.stdout_path[0] != '\0')
        out_entries[count++] = (Dmod_StreamRedirection_t){ .StdHandle = DMOD_STDOUT, .Path = unit->streams.stdout_path };
    if (unit->streams.stderr_path[0] != '\0')
        out_entries[count++] = (Dmod_StreamRedirection_t){ .StdHandle = DMOD_STDERR, .Path = unit->streams.stderr_path };
    if (unit->streams.stdlog_path[0] != '\0')
        out_entries[count++] = (Dmod_StreamRedirection_t){ .StdHandle = DMOD_STDLOG, .Path = unit->streams.stdlog_path };

    return (Dmod_StreamRedirections_t){ .Entries = out_entries, .Count = count };
}

/**
 * @brief Spawns @p unit's module, honoring its stream redirections
 *
 * @return The new process's pid, or a negative error code on failure
 */
static int spawn_with_streams(dmsystem_unit_t* unit)
{
    Dmod_StreamRedirection_t entries[4];
    Dmod_StreamRedirections_t streams = build_streams(unit, entries);

    return Dmod_SpawnModule(unit->exec, unit->argc, unit->argv, streams.Count > 0 ? &streams : NULL);
}

/**
 * @brief Spawns a "simple" (long-running) unit and leaves it running
 */
static bool spawn_unit(dmsystem_unit_t* unit)
{
    int pid = spawn_with_streams(unit);
    if (pid < 0)
    {
        DMOD_LOG_ERROR("Failed to start unit '%s' (exec '%s'): %d\n", unit->name, unit->exec, pid);
        unit->state = DMSYSTEM_UNIT_STATE_FAILED;
        unit->exit_status = pid;
        return false;
    }

    unit->pid = pid;
    unit->state = DMSYSTEM_UNIT_STATE_RUNNING;
    DMOD_LOG_INFO("Started unit '%s' (exec '%s') with pid %d\n", unit->name, unit->exec, pid);
    return true;
}

/**
 * @brief Runs a "oneshot" unit to completion
 *
 * Spawns rather than using Dmod_RunModule (which does not accept stream
 * redirections at all) so that a oneshot unit's stdin/stdout/stderr/stdlog keys
 * are honored exactly like a simple unit's, then blocks until it terminates.
 */
static bool run_oneshot(dmsystem_unit_t* unit)
{
    DMOD_LOG_INFO("Running oneshot unit '%s' (exec '%s')\n", unit->name, unit->exec);

    int pid = spawn_with_streams(unit);
    if (pid < 0)
    {
        DMOD_LOG_ERROR("Failed to start oneshot unit '%s' (exec '%s'): %d\n", unit->name, unit->exec, pid);
        unit->state = DMSYSTEM_UNIT_STATE_FAILED;
        unit->exit_status = pid;
        return false;
    }

    dmosi_process_t proc = dmosi_process_find_by_id((dmosi_process_id_t)pid);
    if (!proc)
    {
        DMOD_LOG_ERROR("Oneshot unit '%s' (pid %d) could not be tracked for waiting\n", unit->name, pid);
        unit->state = DMSYSTEM_UNIT_STATE_FAILED;
        unit->exit_status = -ESRCH;
        return false;
    }

    dmosi_process_wait(proc, -1);
    unit->exit_status = dmosi_process_get_exit_status(proc);
    dmosi_process_destroy(proc);

    if (unit->exit_status != 0)
    {
        DMOD_LOG_ERROR("Oneshot unit '%s' exited with status %d\n", unit->name, unit->exit_status);
        unit->state = DMSYSTEM_UNIT_STATE_FAILED;
        return false;
    }

    unit->state = DMSYSTEM_UNIT_STATE_DONE;
    return true;
}

bool dmsystem_proc_start(dmsystem_unit_t* unit)
{
    if (!unit)
        return false;

    return (unit->type == DMSYSTEM_UNIT_TYPE_ONESHOT) ? run_oneshot(unit) : spawn_unit(unit);
}

bool dmsystem_proc_poll(dmsystem_unit_t* unit)
{
    if (!unit || unit->type != DMSYSTEM_UNIT_TYPE_SIMPLE || unit->state != DMSYSTEM_UNIT_STATE_RUNNING)
        return false;

    dmosi_process_t proc = dmosi_process_find_by_id((dmosi_process_id_t)unit->pid);
    if (!proc)
        return false; /* not tracked by dmosi - nothing to report yet */

    if (dmosi_process_get_state(proc) != DMOSI_PROCESS_STATE_TERMINATED)
        return false;

    unit->exit_status = dmosi_process_get_exit_status(proc);
    unit->state = (unit->exit_status == 0) ? DMSYSTEM_UNIT_STATE_DONE : DMSYSTEM_UNIT_STATE_FAILED;
    dmosi_process_destroy(proc);

    DMOD_LOG_INFO("Unit '%s' terminated with status %d\n", unit->name, unit->exit_status);
    return true;
}
