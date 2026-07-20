#include "dmod.h"
#include "libsystemd.h"
#include <errno.h>
#include <string.h>

/**
 * @brief Print the usage/help banner for this CLI
 */
static void print_usage(const char* program_name)
{
    Dmod_Printf("Usage: %s <list|status|start|stop|restart> [unit_name]\n", program_name);
    Dmod_Printf("\n");
    Dmod_Printf("  list                 Show every unit systemd is currently managing\n");
    Dmod_Printf("  status [unit_name]   Show one unit's status, or every unit's if omitted\n");
    Dmod_Printf("  start   <unit_name>  Start a unit\n");
    Dmod_Printf("  stop    <unit_name>  Stop a unit\n");
    Dmod_Printf("  restart <unit_name>  Stop then start a unit\n");
}

/**
 * @brief Human-readable label for a dmosi_process_state_t value
 */
static const char* state_to_string(dmosi_process_state_t state)
{
    switch (state)
    {
        case DMOSI_PROCESS_STATE_CREATED:    return "created";
        case DMOSI_PROCESS_STATE_RUNNING:    return "running";
        case DMOSI_PROCESS_STATE_SUSPENDED:  return "suspended";
        case DMOSI_PROCESS_STATE_TERMINATED: return "terminated";
        case DMOSI_PROCESS_STATE_ZOMBIE:     return "zombie";
        default:                             return "unknown";
    }
}

/**
 * @brief Human-readable label for a libsystemd_service_type_t value
 */
static const char* type_to_string(libsystemd_service_type_t type)
{
    switch (type)
    {
        case LIBSYSTEMD_SERVICE_TYPE_ONESHOT: return "oneshot";
        case LIBSYSTEMD_SERVICE_TYPE_SIMPLE:
        default:                              return "simple";
    }
}

/**
 * @brief Human-readable label for a libsystemd_restart_policy_t value
 */
static const char* restart_policy_to_string(libsystemd_restart_policy_t policy)
{
    switch (policy)
    {
        case LIBSYSTEMD_RESTART_ALWAYS:     return "always";
        case LIBSYSTEMD_RESTART_ON_FAILURE: return "on-failure";
        case LIBSYSTEMD_RESTART_NO:
        default:                            return "no";
    }
}

static void print_service_info(const libsystemd_service_info_t* info)
{
    if (info->description != NULL)
    {
        Dmod_Printf("%-20s %-10s pid=%-6u %s\n", info->unit_name, state_to_string(info->status.state), info->status.pid, info->description);
    }
    else
    {
        Dmod_Printf("%-20s %-10s pid=%u\n", info->unit_name, state_to_string(info->status.state), info->status.pid);
    }
}

/**
 * @brief Print the "type"/"restart" detail line shown by `service status <unit_name>`
 *
 * Kept separate from print_service_info() since cmd_list() (which reuses
 * print_service_info() for every unit) would get too noisy with this on every
 * line - only a single-unit `service status` query prints it.
 */
static void print_service_detail(const libsystemd_service_info_t* info)
{
    Dmod_Printf("  type=%s restart=%s\n", type_to_string(info->type), restart_policy_to_string(info->restart_policy));
}

/**
 * @brief Closure for count_and_print_visitor(), used by cmd_list()
 */
typedef struct
{
    int count;
} list_state_t;

static bool count_and_print_visitor(const libsystemd_service_info_t* info, void* user_ptr)
{
    list_state_t* state = (list_state_t*)user_ptr;
    state->count++;
    print_service_info(info);
    return true;
}

/**
 * @brief Implements `service list`
 */
static int cmd_list(void)
{
    list_state_t state = { .count = 0 };
    libsystemd_list(count_and_print_visitor, &state);

    if (state.count == 0)
    {
        Dmod_Printf("No units are currently registered.\n");
    }

    return 0;
}

/**
 * @brief Closure for find_service_info_visitor(), used by cmd_status()
 */
typedef struct
{
    const char* unit_name;
    bool found;
    libsystemd_service_info_t info;
} find_info_state_t;

static bool find_service_info_visitor(const libsystemd_service_info_t* info, void* user_ptr)
{
    find_info_state_t* state = (find_info_state_t*)user_ptr;

    if (strcmp(info->unit_name, state->unit_name) == 0)
    {
        state->info = *info;
        state->found = true;
        return false; /* stop iterating - found it */
    }

    return true;
}

/**
 * @brief Implements `service status [unit_name]`
 *
 * With no unit name, behaves exactly like cmd_list(). With one, goes through
 * libsystemd_list() (rather than libsystemd_status()) so the "description"/
 * "type"/"restart" unit metadata - not carried by libsystemd_status()'s
 * process-only libsystemd_service_status_t - can be shown too.
 */
static int cmd_status(const char* unit_name)
{
    if (unit_name == NULL)
    {
        return cmd_list();
    }

    find_info_state_t state = { .unit_name = unit_name, .found = false };
    libsystemd_list(find_service_info_visitor, &state);

    if (!state.found)
    {
        Dmod_Printf("service: unit '%s' not found (%d)\n", unit_name, -ENOENT);
        return -ENOENT;
    }

    print_service_info(&state.info);
    print_service_detail(&state.info);

    return 0;
}

/**
 * @brief Implements `service start <unit_name>`
 */
static int cmd_start(const char* unit_name)
{
    if (unit_name == NULL)
    {
        Dmod_Printf("service: 'start' requires a unit name\n");
        return -EINVAL;
    }

    int result = libsystemd_start_service(unit_name, NULL);
    if (result != 0)
    {
        Dmod_Printf("service: failed to start '%s' (%d)\n", unit_name, result);
    }

    return result;
}

/**
 * @brief Implements `service stop <unit_name>`
 */
static int cmd_stop(const char* unit_name)
{
    if (unit_name == NULL)
    {
        Dmod_Printf("service: 'stop' requires a unit name\n");
        return -EINVAL;
    }

    int result = libsystemd_stop_service(unit_name);
    if (result != 0)
    {
        Dmod_Printf("service: failed to stop '%s' (%d)\n", unit_name, result);
    }

    return result;
}

/**
 * @brief Implements `service restart <unit_name>` (stop, tolerating "not running", then start)
 */
static int cmd_restart(const char* unit_name)
{
    if (unit_name == NULL)
    {
        Dmod_Printf("service: 'restart' requires a unit name\n");
        return -EINVAL;
    }

    int stop_result = libsystemd_stop_service(unit_name);
    if (stop_result != 0 && stop_result != -ESRCH)
    {
        Dmod_Printf("service: failed to stop '%s' before restart (%d)\n", unit_name, stop_result);
        return stop_result;
    }

    return cmd_start(unit_name);
}

/**
 * @brief Entry point of the `service` application module
 *
 * A small systemctl/service-alike CLI over `libsystemd`'s control API - see
 * cmd_list()/cmd_status()/cmd_start()/cmd_stop()/cmd_restart() for what each
 * subcommand does.
 *
 * @param argc Number of arguments; must be at least 2 (program name + command).
 * @param argv Argument vector; `argv[1]` is the subcommand, `argv[2]` (if
 *              present) is the unit name it applies to.
 *
 * @return 0 on success.
 * @retval -EINVAL Missing/unknown subcommand, or a required unit name was omitted.
 * @retval <0      Any other negative value forwarded from the underlying
 *                   libsystemd_start_service()/libsystemd_stop_service()/libsystemd_status() call.
 *
 * @par Example
 * @code
 * dmod_loader service.dmf --args "list"
 * dmod_loader service.dmf --args "status webserver"
 * dmod_loader service.dmf --args "restart webserver"
 * @endcode
 */
int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        print_usage((argc > 0) ? argv[0] : "service");
        return -EINVAL;
    }

    const char* command = argv[1];
    const char* unit_name = (argc >= 3) ? argv[2] : NULL;

    if (strcmp(command, "list") == 0)
    {
        return cmd_list();
    }
    else if (strcmp(command, "status") == 0)
    {
        return cmd_status(unit_name);
    }
    else if (strcmp(command, "start") == 0)
    {
        return cmd_start(unit_name);
    }
    else if (strcmp(command, "stop") == 0)
    {
        return cmd_stop(unit_name);
    }
    else if (strcmp(command, "restart") == 0)
    {
        return cmd_restart(unit_name);
    }

    print_usage(argv[0]);
    return -EINVAL;
}
