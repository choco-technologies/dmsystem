#ifndef DMSYSTEM_CONFIG_H
#define DMSYSTEM_CONFIG_H

#include "dmsystem_unit.h"

/**
 * @file dmsystem_config.h
 * @brief Discovers unit files in a directory and loads them into a dmsystem_unit_list_t.
 *
 * Plain internal header - unlike dmsystem_unit.h/dmsystem_graph.h, this is only ever
 * called from within dmsystem_core.c (in the same module), so it is a regular extern
 * declaration, not a DMOD module API macro.
 *
 * Each service owns its own unit file - there is no single master config to hand-edit
 * or generate. Every "*.ini" file directly inside the given directory is one unit,
 * named after its filename with the extension stripped (e.g. "webserver.ini" becomes
 * the unit "webserver"); its keys are read from the file's global section (no
 * "[section]" header needed, since the file itself is the unit). Subdirectories and
 * non-".ini" entries are ignored; the directory is not searched recursively.
 */

/**
 * @brief Scans @p units_dir for "*.ini" unit files and fills @p out_list with one unit each
 *
 * @param units_dir Path to the directory containing unit files
 * @param out_list  Receives the parsed units (re-initialized by this call)
 * @return 0 on success (even if some individual unit files were malformed and skipped),
 *         or a negative errno-style code if @p units_dir itself could not be opened
 */
int dmsystem_load_config(const char* units_dir, dmsystem_unit_list_t* out_list);

#endif /* DMSYSTEM_CONFIG_H */
