#define DMOD_ENABLE_REGISTRATION ON
#include "dmsystem_unit.h"
#include <string.h>

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

void dmsystem_unit_list_init(dmsystem_unit_list_t* list)
{
    if (!list)
        return;

    memset(list, 0, sizeof(*list));
}

dmsystem_unit_t* dmsystem_unit_list_add(dmsystem_unit_list_t* list, const char* name)
{
    if (!list || !name || name[0] == '\0')
    {
        DMOD_LOG_ERROR("Cannot add unit: invalid list or name\n");
        return NULL;
    }

    if (list->count >= DMSYSTEM_MAX_UNITS)
    {
        DMOD_LOG_ERROR("Cannot add unit '%s': list is full (max %d)\n", name, DMSYSTEM_MAX_UNITS);
        return NULL;
    }

    dmsystem_unit_t* unit = &list->units[list->count];
    memset(unit, 0, sizeof(*unit));

    strncpy(unit->name, name, sizeof(unit->name) - 1);
    unit->name[sizeof(unit->name) - 1] = '\0';
    unit->state = DMSYSTEM_UNIT_STATE_PENDING;

    list->count++;
    return unit;
}

dmsystem_unit_t* dmsystem_unit_list_find(dmsystem_unit_list_t* list, const char* name)
{
    if (!list || !name)
        return NULL;

    for (size_t i = 0; i < list->count; i++)
    {
        if (strcmp(list->units[i].name, name) == 0)
            return &list->units[i];
    }

    return NULL;
}

/**
 * @brief Checks whether a character separates dependency-list entries
 */
static bool is_dep_separator(char c)
{
    return c == ',' || c == ';' || c == ' ' || c == '\t';
}

size_t dmsystem_unit_parse_dependency_list(const char* text, char out[][DMOD_MAX_MODULE_NAME_LENGTH], size_t max_entries)
{
    size_t count = 0;
    if (!text)
        return 0;

    const char* p = text;
    while (*p != '\0' && count < max_entries)
    {
        while (*p != '\0' && is_dep_separator(*p))
            p++;

        if (*p == '\0')
            break;

        const char* start = p;
        while (*p != '\0' && !is_dep_separator(*p))
            p++;

        size_t len = (size_t)(p - start);
        if (len >= DMOD_MAX_MODULE_NAME_LENGTH)
            len = DMOD_MAX_MODULE_NAME_LENGTH - 1;

        memcpy(out[count], start, len);
        out[count][len] = '\0';
        count++;
    }

    return count;
}

/**
 * @brief Checks whether a character separates argv tokens in an "args=" value
 */
static bool is_arg_separator(char c)
{
    return c == ' ' || c == '\t';
}

int dmsystem_unit_build_argv(dmsystem_unit_t* unit, const char* exec, const char* args)
{
    if (!unit)
    {
        DMOD_LOG_ERROR("Cannot build argv: unit is NULL\n");
        return 0;
    }

    strncpy(unit->exec, exec ? exec : "", sizeof(unit->exec) - 1);
    unit->exec[sizeof(unit->exec) - 1] = '\0';

    strncpy(unit->args_storage, args ? args : "", sizeof(unit->args_storage) - 1);
    unit->args_storage[sizeof(unit->args_storage) - 1] = '\0';

    int argc = 0;
    unit->argv[argc++] = unit->exec;

    char* cursor = unit->args_storage;
    while (*cursor != '\0' && argc < DMSYSTEM_MAX_ARGS - 1)
    {
        while (*cursor != '\0' && is_arg_separator(*cursor))
            cursor++;

        if (*cursor == '\0')
            break;

        unit->argv[argc++] = cursor;

        while (*cursor != '\0' && !is_arg_separator(*cursor))
            cursor++;

        if (*cursor != '\0')
        {
            *cursor = '\0';
            cursor++;
        }
    }

    unit->argv[argc] = NULL;
    unit->argc = argc;
    return argc;
}

dmsystem_unit_type_t dmsystem_unit_parse_type(const char* text)
{
    if (text && strcmp(text, "oneshot") == 0)
        return DMSYSTEM_UNIT_TYPE_ONESHOT;

    return DMSYSTEM_UNIT_TYPE_SIMPLE;
}

dmsystem_restart_policy_t dmsystem_unit_parse_restart(const char* text)
{
    if (text && strcmp(text, "always") == 0)
        return DMSYSTEM_RESTART_ALWAYS;

    return DMSYSTEM_RESTART_NO;
}
