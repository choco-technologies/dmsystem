#define DMOD_ENABLE_REGISTRATION ON
#include "dmsystem_graph.h"

/**
 * @brief Plain (non-DMOD-API) declaration of dmsystem_unit_list_find
 *
 * dmsystem_unit_list_find is implemented in dmsystem_unit.c, linked into this same
 * dmsystem_unit.dmf binary, so an ordinary extern declaration resolved at link time
 * is all that's needed here - including dmsystem_unit.h instead would re-register
 * every one of its functions a second time in this translation unit (since
 * DMOD_ENABLE_REGISTRATION is a whole-file setting - see dmsystem_unit.h's note),
 * colliding with their real registrations in dmsystem_unit.c.
 */
extern dmsystem_unit_t* dmsystem_unit_list_find(dmsystem_unit_list_t* list, const char* name);

/**
 * @brief Checks whether every entry in a unit's dependency name array is satisfied
 *
 * A name that does not match any unit in @p list is treated as already satisfied
 * (there is nothing to wait for); a name that does match is satisfied only once
 * that unit has been placed.
 */
static bool deps_satisfied(dmsystem_unit_list_t* list, const char names[][DMOD_MAX_MODULE_NAME_LENGTH],
                            size_t name_count, const bool placed[])
{
    for (size_t i = 0; i < name_count; i++)
    {
        dmsystem_unit_t* dep = dmsystem_unit_list_find(list, names[i]);
        if (!dep)
            continue; /* unknown dependency name - nothing to wait for */

        size_t dep_index = (size_t)(dep - list->units);
        if (!placed[dep_index])
            return false;
    }

    return true;
}

size_t dmsystem_unit_topo_sort(dmsystem_unit_list_t* list, size_t out_order[], size_t max_order)
{
    if (!list || !out_order)
        return 0;

    size_t n = list->count;
    bool placed[DMSYSTEM_MAX_UNITS] = {0};
    size_t order_count = 0;

    for (size_t pass = 0; pass < n; pass++)
    {
        bool progressed = false;

        for (size_t i = 0; i < n; i++)
        {
            if (placed[i])
                continue;

            dmsystem_unit_t* unit = &list->units[i];
            if (!deps_satisfied(list, unit->after, unit->after_count, placed) ||
                !deps_satisfied(list, unit->requires, unit->requires_count, placed))
                continue;

            placed[i] = true;
            progressed = true;

            if (order_count < max_order)
                out_order[order_count] = i;
            order_count++;
        }

        if (!progressed)
            break; /* every remaining unit is waiting on something still unplaced: a cycle */
    }

    return (order_count > max_order) ? max_order : order_count;
}
