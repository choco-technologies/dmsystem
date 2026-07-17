#define DMOD_ENABLE_REGISTRATION ON
#include "dmsystem_core.h"

#include "dmod.h"
#include "dmsystem_unit.h"
#include "dmsystem_graph.h"
#include "dmsystem_config.h"
#include "dmsystem_proc.h"
#include "dmosi.h"
#include <errno.h>

/** @brief How often the supervise loop polls running units for termination */
#define DMSYSTEM_POLL_INTERVAL_MS 1000

/**
 * @brief Module initialization (required for a Library-type DMOD module)
 */
int dmod_init(const Dmod_Config_t* Config)
{
    (void)Config;
    return 0;
}

/**
 * @brief Module deinitialization (required for a Library-type DMOD module)
 */
int dmod_deinit(void)
{
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

    size_t running = start_units_in_order(list, order, order_count);

    while (running > 0)
    {
        dmosi_thread_sleep(DMSYSTEM_POLL_INTERVAL_MS);
        running = supervise_pass(list);
    }

    int failed = count_failed(list);
    Dmod_Free(list);
    return failed;
}
