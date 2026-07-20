#include "dmod.h"
#include "libsystemd.h"
#include <errno.h>
#include <string.h>

/**
 * @brief Print the usage/help banner for this CLI
 *
 * @param program_name Name to show in the usage line (typically argv[0]).
 */
static void print_usage(const char* program_name)
{
    Dmod_Printf("Usage: %s <units-directory> [rules-directory]\n", program_name);
    Dmod_Printf("\n");
    Dmod_Printf("Recursively scans <units-directory> (including subdirectories) for\n");
    Dmod_Printf("\"*.ini\" unit files, resolves their \"requires\"/\"after\" dependency\n");
    Dmod_Printf("order, and starts every unit found there in that order (see\n");
    Dmod_Printf("libsystemd_scan()).\n");
    Dmod_Printf("\n");
    Dmod_Printf("If [rules-directory] is given, it is recursively scanned (including\n");
    Dmod_Printf("subdirectories) for \"*.ini\"/\"*.rules\" device-class rule files, which\n");
    Dmod_Printf("are loaded so devices reported at runtime can be started/stopped\n");
    Dmod_Printf("automatically (see libsystemd_load_rules()).\n");
    Dmod_Printf("\n");
    Dmod_Printf("Options:\n");
    Dmod_Printf("  -h, --help    Show this help message and exit\n");
}

/**
 * @brief Entry point of the `systemd` application module
 *
 * A thin front-end over `libsystemd`: parses the command line, then delegates
 * the actual unit-file parsing, dependency-ordering and starting to
 * libsystemd_scan(), and - if a rules directory was given - the loading of
 * device-class rules to libsystemd_load_rules(). Long-running units keep
 * running as independently spawned processes after this function returns -
 * `main()` itself does not block or supervise them, it only performs the
 * initial scan-and-start pass.
 *
 * @param argc Number of arguments; must be 2 (the program name plus either
 *              the units directory path or `-h`/`--help`) or 3 (the program
 *              name plus the units directory and a rules directory).
 * @param argv Argument vector; `argv[1]` is the units directory to scan, or
 *              `-h`/`--help` to print usage and exit. `argv[2]`, if given, is
 *              a directory of device-class rule files to load.
 *
 * @return 0 on success (including `--help`).
 * @retval -EINVAL The units directory argument was missing/malformed (wrong argc);
 *                   usage is printed to help diagnose this.
 * @retval <0      Any other negative value is forwarded from libsystemd_scan()
 *                   or libsystemd_load_rules() (e.g. -ENOENT if a directory
 *                   does not exist).
 *
 * @par Example
 * @code
 * dmod_loader systemd.dmf --args "/etc/dmsystem/units"
 * dmod_loader systemd.dmf --args "/etc/dmsystem/units /etc/dmsystem/rules"
 * dmod_loader systemd.dmf --args "--help"
 * @endcode
 */
int main(int argc, char *argv[])
{
    const char* program_name = (argc > 0) ? argv[0] : "systemd";

    if (argc != 2 && argc != 3)
    {
        print_usage(program_name);
        return -EINVAL;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
    {
        print_usage(program_name);
        return 0;
    }

    const char* units_dir = argv[1];

    int result = libsystemd_scan(units_dir);
    if (result != 0)
    {
        Dmod_Printf("systemd: failed to scan '%s' (%d)\n", units_dir, result);
        return result;
    }

    Dmod_Printf("systemd: scanned and started units from '%s'\n", units_dir);

    if (argc == 3)
    {
        const char* rules_dir = argv[2];

        result = libsystemd_load_rules(rules_dir);
        if (result != 0)
        {
            Dmod_Printf("systemd: failed to load rules from '%s' (%d)\n", rules_dir, result);
            return result;
        }

        Dmod_Printf("systemd: loaded device rules from '%s'\n", rules_dir);
    }

    return 0;
}
