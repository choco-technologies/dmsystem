#include "dmod.h"
#include "dmsystem_core.h"
#include <errno.h>
#include <string.h>

/**
 * @file service.c
 * @brief `service` - a systemctl/`service`-alike CLI for inspecting and controlling
 *        the units a running dmsystem is managing.
 *
 * Talks to dmsystem_core directly, not to dmsystem itself: dmsystem_core is a Library
 * module loaded once and shared by every Application that requires it, so `service`
 * (a separate Application, requiring dmsystem_core exactly like dmsystem does) reaches
 * the very same in-memory unit list dmsystem's supervise loop is managing - there is
 * no separate daemon/socket to talk to. If dmsystem is not currently running,
 * dmsystem_core reports zero units and every command below fails accordingly.
 */

/**
 * @brief Human-readable name for a unit state, for `service status` output
 */
static const char* state_name(dmsystem_unit_state_t state)
{
    switch (state)
    {
        case DMSYSTEM_UNIT_STATE_PENDING: return "pending";
        case DMSYSTEM_UNIT_STATE_RUNNING: return "running";
        case DMSYSTEM_UNIT_STATE_DONE:    return "done";
        case DMSYSTEM_UNIT_STATE_FAILED:  return "failed";
        case DMSYSTEM_UNIT_STATE_SKIPPED: return "skipped";
        case DMSYSTEM_UNIT_STATE_STOPPED: return "stopped";
        default:                          return "unknown";
    }
}

/**
 * @brief Prints one unit's status line
 */
static void print_status(const dmsystem_unit_status_t* status)
{
    Dmod_Printf("%-20s %-8s pid=%-6d exit=%-4d %s\n",
                status->name, state_name(status->state),
                (int)status->pid, status->exit_status, status->description);
}

/**
 * @brief `service status [unit]` - show one unit, or every unit if none was named
 */
static int cmd_status(const char* unit_name)
{
    if (unit_name)
    {
        dmsystem_unit_status_t status;
        if (!dmsystem_core_find_unit_status(unit_name, &status))
        {
            Dmod_Printf("Unit '%s' not found (is dmsystem running, and does it manage this unit?)\n", unit_name);
            return 1;
        }

        print_status(&status);
        return 0;
    }

    size_t count = dmsystem_core_get_unit_count();
    if (count == 0)
    {
        Dmod_Printf("No units loaded (is dmsystem running?)\n");
        return 0;
    }

    for (size_t i = 0; i < count; i++)
    {
        dmsystem_unit_status_t status;
        if (dmsystem_core_get_unit_status(i, &status))
            print_status(&status);
    }

    return 0;
}

/**
 * @brief `service list` - a compact listing of every unit's name/type/exec
 *
 * Unlike `status` (which shows each unit's live state, pid and exit code),
 * `list` is a quick overview of what's configured, regardless of whether it is
 * currently running.
 */
static int cmd_list(void)
{
    size_t count = dmsystem_core_get_unit_count();
    if (count == 0)
    {
        Dmod_Printf("No units loaded (is dmsystem running?)\n");
        return 0;
    }

    Dmod_Printf("%-20s %-8s %-20s %s\n", "UNIT", "TYPE", "EXEC", "DESCRIPTION");

    for (size_t i = 0; i < count; i++)
    {
        dmsystem_unit_status_t status;
        if (!dmsystem_core_get_unit_status(i, &status))
            continue;

        const char* type = (status.type == DMSYSTEM_UNIT_TYPE_ONESHOT) ? "oneshot" : "simple";
        Dmod_Printf("%-20s %-8s %-20s %s\n", status.name, type, status.exec, status.description);
    }

    return 0;
}

/**
 * @brief `service start|stop|restart <unit>`
 */
static int cmd_control(const char* action, const char* unit_name)
{
    if (!unit_name)
    {
        DMOD_LOG_ERROR("Usage: service %s <unit>\n", action);
        return -EINVAL;
    }

    bool ok;
    const char* past_tense;

    if (strcmp(action, "start") == 0)
    {
        ok = dmsystem_core_start_unit(unit_name);
        past_tense = "started";
    }
    else if (strcmp(action, "stop") == 0)
    {
        ok = dmsystem_core_stop_unit(unit_name);
        past_tense = "stopped";
    }
    else
    {
        ok = dmsystem_core_restart_unit(unit_name);
        past_tense = "restarted";
    }

    if (!ok)
    {
        Dmod_Printf("Failed to %s unit '%s'\n", action, unit_name);
        return 1;
    }

    Dmod_Printf("Unit '%s' %s\n", unit_name, past_tense);
    return 0;
}

static void print_usage(void)
{
    Dmod_Printf("Usage: service <command> [unit]\n");
    Dmod_Printf("Commands:\n");
    Dmod_Printf("  list             List every unit dmsystem knows about (name, type, exec)\n");
    Dmod_Printf("  status [unit]    Show live status of one unit, or every unit if omitted\n");
    Dmod_Printf("  start <unit>     Start a unit if it is not already running\n");
    Dmod_Printf("  stop <unit>      Stop a running unit\n");
    Dmod_Printf("  restart <unit>   Stop (if running) and start a unit\n");
}

int main(int argc, char** argv)
{
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
    {
        print_usage();
        return (argc < 2) ? -EINVAL : 0;
    }

    const char* command = argv[1];
    const char* unit_name = (argc > 2) ? argv[2] : NULL;

    if (strcmp(command, "list") == 0)
        return cmd_list();

    if (strcmp(command, "status") == 0)
        return cmd_status(unit_name);

    if (strcmp(command, "start") == 0 || strcmp(command, "stop") == 0 || strcmp(command, "restart") == 0)
        return cmd_control(command, unit_name);

    DMOD_LOG_ERROR("Unknown command '%s'\n", command);
    print_usage();
    return -EINVAL;
}
