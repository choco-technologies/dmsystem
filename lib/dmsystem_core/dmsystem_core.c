#define DMOD_ENABLE_REGISTRATION ON
#include "dmsystem_core.h"

#include "dmod.h"
#include "dmsystem_unit.h"
#include "dmsystem_graph.h"
#include "dmsystem_config.h"
#include "dmsystem_proc.h"
#include "dmosi.h"
#include <errno.h>
#include <string.h>

/** @brief How often the supervise loop polls running units for termination */
#define DMSYSTEM_POLL_INTERVAL_MS 1000

/**
 * @brief The unit list dmsystem_core_run is currently supervising, or NULL if it
 *        has not been started (or has already finished)
 *
 * Set/cleared under g_lock by dmsystem_core_run itself; read and mutated under
 * g_lock by the get/find/start/stop/restart_unit query/control API, which runs on
 * whatever thread/process `service` (or any other caller) happens to be on.
 */
static dmsystem_unit_list_t* g_units = NULL;

/**
 * @brief Guards all access to g_units
 *
 * @note dmsystem_proc_start() blocks for the duration of a oneshot unit's entire
 *       run (see dmsystem_proc.h), and every path that calls it here does so with
 *       g_lock held - so a slow oneshot unit (whether started by dmsystem_core_run's
 *       own startup pass, or manually via dmsystem_core_start_unit/restart_unit)
 *       delays the supervise loop and any concurrent `service` call for as long as
 *       it runs. Acceptable for oneshot units, which are expected to be quick
 *       (migrations and the like), not for general use.
 */
static dmosi_mutex_t g_lock = NULL;

/**
 * @brief Module initialization (required for a Library-type DMOD module)
 */
int dmod_init(const Dmod_Config_t* Config)
{
    (void)Config;

    g_lock = dmosi_mutex_create(false);
    if (!g_lock)
    {
        DMOD_LOG_ERROR("Failed to create dmsystem_core's unit-list mutex\n");
        return -ENOMEM;
    }

    return 0;
}

/**
 * @brief Module deinitialization (required for a Library-type DMOD module)
 */
int dmod_deinit(void)
{
    if (g_lock)
    {
        dmosi_mutex_destroy(g_lock);
        g_lock = NULL;
    }

    return 0;
}

/**
 * @brief Marks every unit not present in the first @p order_count entries of @p order
 *        as SKIPPED - these are exactly the units dmsystem_unit_topo_sort could not
 *        place, i.e. the ones caught in a dependency cycle.
 */
static void mark_skipped_by_cycle(dmsystem_unit_list_t* list, const size_t order[], size_t order_count)
{
    bool placed[DMSYSTEM_MAX_UNITS] = {0};
    for (size_t i = 0; i < order_count; i++)
        placed[order[i]] = true;

    for (size_t i = 0; i < list->count; i++)
    {
        if (!placed[i])
        {
            DMOD_LOG_ERROR("Unit '%s' is part of a dependency cycle, skipping\n", list->units[i].name);
            list->units[i].state = DMSYSTEM_UNIT_STATE_SKIPPED;
        }
    }
}

/**
 * @brief Checks whether any of @p unit's hard ("requires") dependencies failed or were skipped
 */
static bool required_dep_failed(dmsystem_unit_list_t* list, const dmsystem_unit_t* unit)
{
    for (size_t i = 0; i < unit->requires_count; i++)
    {
        dmsystem_unit_t* dep = dmsystem_unit_list_find(list, unit->requires[i]);
        if (dep && (dep->state == DMSYSTEM_UNIT_STATE_FAILED || dep->state == DMSYSTEM_UNIT_STATE_SKIPPED))
            return true;
    }

    return false;
}

/**
 * @brief Starts every unit in @p order, skipping those whose hard dependencies failed
 *
 * @return Number of units left in the RUNNING state (i.e. "simple" units to supervise)
 */
static size_t start_units_in_order(dmsystem_unit_list_t* list, const size_t order[], size_t order_count)
{
    size_t running = 0;

    for (size_t i = 0; i < order_count; i++)
    {
        dmsystem_unit_t* unit = &list->units[order[i]];

        if (required_dep_failed(list, unit))
        {
            DMOD_LOG_WARN("Skipping unit '%s': a required dependency failed\n", unit->name);
            unit->state = DMSYSTEM_UNIT_STATE_SKIPPED;
            continue;
        }

        dmsystem_proc_start(unit);
        if (unit->state == DMSYSTEM_UNIT_STATE_RUNNING)
            running++;
    }

    return running;
}

/**
 * @brief Polls every RUNNING unit once, restarting terminated ones whose policy asks for it
 *
 * @return Number of units still RUNNING after this pass
 */
static size_t supervise_pass(dmsystem_unit_list_t* list)
{
    size_t running = 0;

    for (size_t i = 0; i < list->count; i++)
    {
        dmsystem_unit_t* unit = &list->units[i];
        if (unit->state != DMSYSTEM_UNIT_STATE_RUNNING)
            continue;

        if (dmsystem_proc_poll(unit) && unit->restart == DMSYSTEM_RESTART_ALWAYS)
        {
            DMOD_LOG_INFO("Restarting unit '%s'\n", unit->name);
            dmsystem_proc_start(unit);
        }

        if (unit->state == DMSYSTEM_UNIT_STATE_RUNNING)
            running++;
    }

    return running;
}

/**
 * @brief Counts units left in the FAILED state, used as this module's exit code
 */
static int count_failed(const dmsystem_unit_list_t* list)
{
    int failed = 0;
    for (size_t i = 0; i < list->count; i++)
        if (list->units[i].state == DMSYSTEM_UNIT_STATE_FAILED)
            failed++;

    return failed;
}

int dmsystem_core_run(const char* units_dir)
{
    /* dmsystem_unit_list_t is tens of KB (DMSYSTEM_MAX_UNITS units, each carrying argv
     * storage, dependency name arrays and stream-redirection paths) - too large to put
     * on the stack of a module whose DMOD_STACK_SIZE is a few KB, so it is heap-allocated
     * here instead. */
    dmsystem_unit_list_t* list = Dmod_Malloc(sizeof(*list));
    if (!list)
    {
        DMOD_LOG_ERROR("Failed to allocate unit list (%zu bytes)\n", sizeof(*list));
        return -ENOMEM;
    }

    int result = dmsystem_load_config(units_dir, list);
    if (result != 0)
    {
        Dmod_Free(list);
        return result;
    }

    DMOD_LOG_INFO("Loaded %zu unit(s) from '%s'\n", list->count, units_dir);

    size_t order[DMSYSTEM_MAX_UNITS];
    size_t order_count = dmsystem_unit_topo_sort(list, order, DMSYSTEM_MAX_UNITS);
    if (order_count < list->count)
        mark_skipped_by_cycle(list, order, order_count);

    /* Published before starting anything, so `service status` can already see
     * PENDING units while startup is still in progress. */
    dmosi_mutex_lock(g_lock);
    g_units = list;
    size_t running = start_units_in_order(list, order, order_count);
    dmosi_mutex_unlock(g_lock);

    while (running > 0)
    {
        dmosi_thread_sleep(DMSYSTEM_POLL_INTERVAL_MS);

        dmosi_mutex_lock(g_lock);
        running = supervise_pass(list);
        dmosi_mutex_unlock(g_lock);
    }

    dmosi_mutex_lock(g_lock);
    int failed = count_failed(list);
    g_units = NULL;
    dmosi_mutex_unlock(g_lock);

    Dmod_Free(list);
    return failed;
}

/**
 * @brief Copies the externally-relevant fields of @p unit into a status snapshot
 */
static void copy_status(const dmsystem_unit_t* unit, dmsystem_unit_status_t* out)
{
    memcpy(out->name, unit->name, sizeof(out->name));
    memcpy(out->description, unit->description, sizeof(out->description));
    memcpy(out->exec, unit->exec, sizeof(out->exec));
    out->type = unit->type;
    out->restart = unit->restart;
    out->state = unit->state;
    out->pid = unit->pid;
    out->exit_status = unit->exit_status;
}

/**
 * @brief Kills @p unit's currently-running process, if dmosi can still find it
 *
 * Leaves unit->state untouched - callers set the appropriate state themselves.
 */
static void kill_running_process(const dmsystem_unit_t* unit)
{
    dmosi_process_t proc = dmosi_process_find_by_id((dmosi_process_id_t)unit->pid);
    if (proc)
    {
        dmosi_process_kill(proc, 0);
        dmosi_process_destroy(proc);
    }
}

size_t dmsystem_core_get_unit_count(void)
{
    size_t count = 0;

    dmosi_mutex_lock(g_lock);
    if (g_units)
        count = g_units->count;
    dmosi_mutex_unlock(g_lock);

    return count;
}

bool dmsystem_core_get_unit_status(size_t index, dmsystem_unit_status_t* out)
{
    if (!out)
        return false;

    bool found = false;

    dmosi_mutex_lock(g_lock);
    if (g_units && index < g_units->count)
    {
        copy_status(&g_units->units[index], out);
        found = true;
    }
    dmosi_mutex_unlock(g_lock);

    return found;
}

bool dmsystem_core_find_unit_status(const char* name, dmsystem_unit_status_t* out)
{
    if (!out || !name)
        return false;

    bool found = false;

    dmosi_mutex_lock(g_lock);
    if (g_units)
    {
        dmsystem_unit_t* unit = dmsystem_unit_list_find(g_units, name);
        if (unit)
        {
            copy_status(unit, out);
            found = true;
        }
    }
    dmosi_mutex_unlock(g_lock);

    return found;
}

bool dmsystem_core_start_unit(const char* name)
{
    if (!name)
        return false;

    bool ok = false;

    dmosi_mutex_lock(g_lock);
    if (g_units)
    {
        dmsystem_unit_t* unit = dmsystem_unit_list_find(g_units, name);
        if (unit)
            ok = (unit->state == DMSYSTEM_UNIT_STATE_RUNNING) || dmsystem_proc_start(unit);
    }
    dmosi_mutex_unlock(g_lock);

    return ok;
}

bool dmsystem_core_stop_unit(const char* name)
{
    if (!name)
        return false;

    bool ok = false;

    dmosi_mutex_lock(g_lock);
    if (g_units)
    {
        dmsystem_unit_t* unit = dmsystem_unit_list_find(g_units, name);
        if (unit && unit->state == DMSYSTEM_UNIT_STATE_RUNNING)
        {
            kill_running_process(unit);
            unit->state = DMSYSTEM_UNIT_STATE_STOPPED;
            unit->exit_status = 0;
            DMOD_LOG_INFO("Stopped unit '%s'\n", name);
            ok = true;
        }
    }
    dmosi_mutex_unlock(g_lock);

    return ok;
}

bool dmsystem_core_restart_unit(const char* name)
{
    if (!name)
        return false;

    bool ok = false;

    dmosi_mutex_lock(g_lock);
    if (g_units)
    {
        dmsystem_unit_t* unit = dmsystem_unit_list_find(g_units, name);
        if (unit)
        {
            if (unit->state == DMSYSTEM_UNIT_STATE_RUNNING)
                kill_running_process(unit);

            DMOD_LOG_INFO("Restarting unit '%s'\n", name);
            ok = dmsystem_proc_start(unit);
        }
    }
    dmosi_mutex_unlock(g_lock);

    return ok;
}
