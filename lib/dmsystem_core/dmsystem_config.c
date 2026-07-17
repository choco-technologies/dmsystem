#include "dmsystem_config.h"
#include "dmod.h"
#include "dmini.h"
#include <errno.h>
#include <string.h>

/** @brief Extension a directory entry must have to be considered a unit file */
#define DMSYSTEM_UNIT_FILE_EXTENSION ".ini"

/**
 * @brief Checks whether @p name ends with DMSYSTEM_UNIT_FILE_EXTENSION
 */
static bool has_ini_extension(const char* name)
{
    size_t name_len = strlen(name);
    size_t ext_len = strlen(DMSYSTEM_UNIT_FILE_EXTENSION);

    return name_len > ext_len && strcmp(name + name_len - ext_len, DMSYSTEM_UNIT_FILE_EXTENSION) == 0;
}

/**
 * @brief Derives a unit name from a unit file's name by stripping its extension
 */
static void derive_unit_name(char* out, size_t out_size, const char* filename)
{
    size_t len = strlen(filename) - strlen(DMSYSTEM_UNIT_FILE_EXTENSION);
    if (len >= out_size)
        len = out_size - 1;

    memcpy(out, filename, len);
    out[len] = '\0';
}

/**
 * @brief Copies one optional path key into @p out, defaulting to "" (unset) if absent
 */
static void load_stream_path(dmini_context_t ctx, const char* key, char* out, size_t out_size)
{
    const char* value = dmini_get_string(ctx, NULL, key, "");
    strncpy(out, value, out_size - 1);
    out[out_size - 1] = '\0';
}

/**
 * @brief Fills in a unit's stdin/stdout/stderr/stdlog redirection paths
 */
static void load_unit_streams(dmini_context_t ctx, dmsystem_streams_t* streams)
{
    load_stream_path(ctx, "stdin", streams->stdin_path, sizeof(streams->stdin_path));
    load_stream_path(ctx, "stdout", streams->stdout_path, sizeof(streams->stdout_path));
    load_stream_path(ctx, "stderr", streams->stderr_path, sizeof(streams->stderr_path));
    load_stream_path(ctx, "stdlog", streams->stdlog_path, sizeof(streams->stdlog_path));
}

/**
 * @brief Fills in a unit's fields from its (already open) unit file's global section
 */
static void load_unit_fields(dmini_context_t ctx, dmsystem_unit_t* unit)
{
    const char* description = dmini_get_string(ctx, NULL, "description", unit->name);
    strncpy(unit->description, description, sizeof(unit->description) - 1);
    unit->description[sizeof(unit->description) - 1] = '\0';

    const char* exec = dmini_get_string(ctx, NULL, "exec", "");
    const char* args = dmini_get_string(ctx, NULL, "args", "");
    dmsystem_unit_build_argv(unit, exec, args);

    unit->type = dmsystem_unit_parse_type(dmini_get_string(ctx, NULL, "type", "simple"));
    unit->restart = dmsystem_unit_parse_restart(dmini_get_string(ctx, NULL, "restart", "no"));
    load_unit_streams(ctx, &unit->streams);

    unit->after_count = dmsystem_unit_parse_dependency_list(
        dmini_get_string(ctx, NULL, "after", ""), unit->after, DMSYSTEM_MAX_DEPS_PER_UNIT);
    unit->requires_count = dmsystem_unit_parse_dependency_list(
        dmini_get_string(ctx, NULL, "requires", ""), unit->requires, DMSYSTEM_MAX_DEPS_PER_UNIT);
}

/**
 * @brief Parses one unit file and appends the resulting unit to @p out_list
 *
 * Failures (bad INI syntax, missing "exec", too many units already loaded) are logged
 * and skipped rather than aborting the whole scan - one broken unit file should not
 * prevent every other service from starting.
 */
static void load_unit_file(const char* dir_path, const char* filename, dmsystem_unit_list_t* out_list)
{
    char full_path[DMOD_MAX_FILE_PATH_LENGTH];
    Dmod_SnPrintf(full_path, sizeof(full_path), "%s/%s", dir_path, filename);

    char unit_name[DMOD_MAX_MODULE_NAME_LENGTH];
    derive_unit_name(unit_name, sizeof(unit_name), filename);

    dmini_context_t ctx = dmini_create();
    if (!ctx)
    {
        DMOD_LOG_ERROR("Failed to allocate INI context for '%s'\n", full_path);
        return;
    }

    int result = dmini_parse_file(ctx, full_path);
    if (result != DMINI_OK)
    {
        DMOD_LOG_ERROR("Failed to parse unit file '%s' (error %d), ignoring\n", full_path, result);
        dmini_destroy(ctx);
        return;
    }

    const char* exec = dmini_get_string(ctx, NULL, "exec", NULL);
    if (!exec || exec[0] == '\0')
    {
        DMOD_LOG_ERROR("Unit file '%s' has no 'exec' key, ignoring\n", full_path);
        dmini_destroy(ctx);
        return;
    }

    dmsystem_unit_t* unit = dmsystem_unit_list_add(out_list, unit_name);
    if (!unit)
    {
        DMOD_LOG_ERROR("Too many units (max %d), ignoring '%s'\n", DMSYSTEM_MAX_UNITS, full_path);
        dmini_destroy(ctx);
        return;
    }

    load_unit_fields(ctx, unit);
    dmini_destroy(ctx);
}

int dmsystem_load_config(const char* units_dir, dmsystem_unit_list_t* out_list)
{
    if (!units_dir || !out_list)
        return -EINVAL;

    dmsystem_unit_list_init(out_list);

    void* dir = Dmod_OpenDir(units_dir);
    if (!dir)
    {
        DMOD_LOG_ERROR("Failed to open units directory '%s'\n", units_dir);
        return -ENOENT;
    }

    const Dmod_DirEntry_t* entry;
    while ((entry = Dmod_ReadDirEx(dir)) != NULL)
    {
        if (entry->type == Dmod_DirEntryType_Dir || !has_ini_extension(entry->name))
            continue;

        load_unit_file(units_dir, entry->name, out_list);
    }

    Dmod_CloseDir(dir);
    return 0;
}
