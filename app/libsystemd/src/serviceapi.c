#define DMOD_ENABLE_REGISTRATION ON
#include "libsystemd.h"
#include <errno.h>
#include <string.h>
#include "dmini.h"
#include "dmosi.h"

/**
 * @brief Full definition of the opaque @ref libsystemd_service_t handle
 *
 * The public API only ever hands out this structure behind the opaque
 * `libsystemd_service_t` pointer declared in libsystemd_types.h, so callers outside
 * this file cannot read or modify its fields directly - they must go through
 * libsystemd_start_service()/libsystemd_stop_service()/libsystemd_status()/libsystemd_list().
 *
 * A `libsystemd_service_t` is created by libsystemd_parse_unit_internal()
 * (called from libsystemd_parse_dir(), via the public libsystemd_parse_file()
 * for a non-templated unit) and lives inside the global service registry
 * (@ref g_services) until it is replaced by a new libsystemd_scan() or the
 * module is unloaded (libsystemd_serviceapi_deinit()).
 */
struct libsystemd_service
{
    char* unit_name;                    //!< Unit name, derived from the ini file name (without the ".ini" suffix). Owned copy.
    char* exec;                         //!< Module name (or file path) to spawn, from the "exec" key. Owned copy.
    int argc;                           //!< Number of entries in argv (always >= 1, argv[0] == exec).
    char** argv;                        //!< NULL-terminated argument vector (argc+1 entries, each an owned copy).
    Dmod_StreamRedirections_t streams;  //!< Stream redirections built from the optional "stdin"/"stdout"/"stderr"/"stdlog" keys.
    int starting_order;                 //!< Relative start order computed by libsystemd_resolve_starting_order() (lower starts first).
    dmlist_context_t* required;         //!< List of owned `char*` unit names this service requires (from "requires").
    dmlist_context_t* after;            //!< List of owned `char*` unit names this service must start after (from "after").
    Dmod_Pid_t pid;                     //!< PID returned by the last successful start, or <= 0 if never started/not running.
};

/**
 * @brief Full definition of the opaque @ref libsystemd_services_t handle
 *
 * A thin wrapper around a dmlist of `libsystemd_service_t` pointers. Kept as its
 * own type (rather than exposing the dmlist directly) so the public API
 * surface in libsystemd.h never needs to depend on dmlist.h.
 */
struct libsystemd_services
{
    dmlist_context_t* services;         //!< List of `libsystemd_service_t` entries (list data pointers are libsystemd_service_t).
};

/**
 * @brief Global service registry populated by libsystemd_scan()
 *
 * Owns every `libsystemd_service_t` currently known to this module instance.
 * Allocated by libsystemd_serviceapi_init() (called automatically from dmod_init()
 * when the module is loaded), repopulated on every successful libsystemd_scan(),
 * and released by libsystemd_serviceapi_deinit() (called automatically from
 * dmod_deinit() when the module is unloaded).
 *
 * NULL before the module has finished initializing, or after it has been
 * deinitialized - every API entry point that reads it (libsystemd_start_service(),
 * libsystemd_stop_service(), libsystemd_status(), libsystemd_list()) tolerates that and
 * reports "not found" rather than crashing.
 *
 * @note This module is not designed for concurrent access from multiple
 *       threads: there is no locking around @ref g_services, so callers that
 *       run their own worker threads must serialize their own calls into this
 *       module's API.
 */
static libsystemd_services_t g_services = NULL;

/**
 * @brief Units directory of the last successful libsystemd_scan(), or NULL
 *
 * Remembered purely so libsystemd_start_service() can resolve an on-demand
 * template instance (e.g. "getty@tty1", with no matching file ever having
 * been scanned) by looking for "<prefix>@.ini" in the same directory - see
 * libsystemd_instantiate_service_on_demand(). NULL before the first
 * successful libsystemd_scan(), or after libsystemd_serviceapi_deinit().
 */
static char* g_units_dir = NULL;

/**
 * @brief Closure passed to libsystemd_dependency_order_visitor() while walking one service's dependency lists
 */
typedef struct
{
    dmlist_context_t* services;  //!< The full service list, searched by unit name for each dependency.
    int max_order;               //!< Highest `(dependency->starting_order + 1)` seen so far.
} libsystemd_order_ctx_t;

/**
 * @brief Closure passed to libsystemd_resolve_service_order_visitor() while walking the whole service list
 */
typedef struct
{
    dmlist_context_t* services;  //!< The full service list (forwarded into @ref libsystemd_order_ctx_t for each service).
    bool changed;                //!< Set to true if any service's starting_order was raised during this pass.
} libsystemd_pass_ctx_t;

/**
 * @brief Closure passed to libsystemd_list_visitor() while walking the service list for libsystemd_list()
 */
typedef struct
{
    libsystemd_visitor_t visitor;  //!< Caller-supplied visitor to invoke for each service.
    void* user_ptr;             //!< Caller-supplied opaque pointer, forwarded verbatim to the visitor.
} libsystemd_list_ctx_t;

/**
 * @brief Free every element of a dependency name list and then the list itself
 *
 * Used to tear down the `required`/`after` lists of a `libsystemd_service_t`, both
 * of which hold heap-allocated `char*` unit names (see libsystemd_parse_name_list()).
 *
 * @param list List of owned `char*` entries to free, or NULL (no-op).
 *
 * @return Nothing.
 *
 * @note Safe to call with `list == NULL`; simply returns immediately.
 *
 * @par Example
 * @code
 * libsystemd_destroy_name_list(service->required);
 * service->required = NULL;
 * @endcode
 */
static void libsystemd_destroy_name_list(dmlist_context_t* list)
{
    if (list == NULL)
    {
        return;
    }

    char* name = (char*)dmlist_pop_front(list);
    while (name != NULL)
    {
        Dmod_Free(name);
        name = (char*)dmlist_pop_front(list);
    }

    dmlist_destroy(list);
}

/**
 * @brief Free a single service and every resource it owns
 *
 * Releases `unit_name`, `exec`, every `argv` entry and the `argv` array itself,
 * every stream redirection path and the `streams.Entries` array, and both the
 * `required` and `after` dependency lists, before finally freeing the
 * `libsystemd_service_t` itself. Does **not** stop the service's process first -
 * callers that may be destroying a running service should call
 * libsystemd_stop_service_internal() beforehand (see libsystemd_stop_all_services()).
 *
 * @param service Service to destroy, or NULL (no-op).
 *
 * @return Nothing.
 *
 * @note Every field is checked for NULL before use, so this is safe to call on
 *       a partially-initialized service (e.g. cleanup after a failed parse).
 *
 * @par Example
 * @code
 * libsystemd_service_t service = NULL;
 * if (libsystemd_parse_file("/etc/services/foo.ini", &service) != 0) {
 *     // parsing failed, nothing to destroy
 * } else if (some_validation_failed) {
 *     libsystemd_destroy_service(service);
 * }
 * @endcode
 */
static void libsystemd_destroy_service(libsystemd_service_t service)
{
    if (service == NULL)
    {
        return;
    }

    Dmod_Free(service->unit_name);
    Dmod_Free(service->exec);

    if (service->argv != NULL)
    {
        for (int i = 0; i < service->argc; i++)
        {
            Dmod_Free(service->argv[i]);
        }
        Dmod_Free(service->argv);
    }

    if (service->streams.Entries != NULL)
    {
        for (size_t i = 0; i < service->streams.Count; i++)
        {
            Dmod_Free((void*)service->streams.Entries[i].Path);
        }
        Dmod_Free((void*)service->streams.Entries);
    }

    libsystemd_destroy_name_list(service->required);
    libsystemd_destroy_name_list(service->after);

    Dmod_Free(service);
}

/**
 * @brief Free every service in a registry and then the registry itself
 *
 * Pops and destroys (via libsystemd_destroy_service()) every entry from
 * `services->services`, destroys the now-empty dmlist, and finally frees the
 * `libsystemd_services_t` wrapper.
 *
 * @param services Registry to destroy, or NULL (no-op).
 *
 * @return Nothing.
 *
 * @note This does **not** stop any running processes associated with the
 *       services first - see libsystemd_stop_all_services(), which callers such
 *       as libsystemd_serviceapi_deinit() and libsystemd_scan() call beforehand.
 *
 * @par Example
 * @code
 * libsystemd_services_t parsed = NULL;
 * libsystemd_parse_dir("/etc/services", &parsed);
 * // ... use parsed ...
 * libsystemd_destroy_services(parsed);
 * @endcode
 */
static void libsystemd_destroy_services(libsystemd_services_t services)
{
    if (services == NULL)
    {
        return;
    }

    if (services->services != NULL)
    {
        libsystemd_service_t service = (libsystemd_service_t)dmlist_pop_front(services->services);
        while (service != NULL)
        {
            libsystemd_destroy_service(service);
            service = (libsystemd_service_t)dmlist_pop_front(services->services);
        }
        dmlist_destroy(services->services);
    }

    Dmod_Free(services);
}

/**
 * @brief Comparator matching a service against a unit name string
 *
 * Intended for use with dmlist_find()/dmlist_find_next(), which invoke the
 * comparator as `compare_func(node_data, query_data)` - so @p data1 is always
 * a `libsystemd_service_t` taken from the list, and @p data2 is always the
 * `const char*` unit name being searched for.
 *
 * @param data1 Node data, actually a `libsystemd_service_t` (`const struct libsystemd_service*`).
 * @param data2 Query data, actually a `const char*` unit name.
 *
 * @retval 0    The service's unit_name equals the queried name.
 * @retval <0   The service's unit_name sorts before the queried name (strcmp order).
 * @retval >0   The service's unit_name sorts after the queried name (strcmp order).
 *
 * @note Not meant to be called directly - pass it as the `compare_func`
 *       argument to dmlist_find()/dmlist_find_next(), as done in
 *       libsystemd_find_service() and libsystemd_dependency_order_visitor().
 */
static int libsystemd_compare_by_unit_name(const void* data1, const void* data2)
{
    const struct libsystemd_service* service = (const struct libsystemd_service*)data1;
    const char* name = (const char*)data2;
    return strcmp(service->unit_name, name);
}

/**
 * @brief Comparator ordering two services by their computed starting_order
 *
 * Intended for use with dmlist_sort(), which sorts ascending - services with
 * a smaller starting_order end up earlier in the list, which is exactly what
 * libsystemd_scan() relies on when it starts services in list order.
 *
 * @param data1 First service, actually a `libsystemd_service_t` (`const struct libsystemd_service*`).
 * @param data2 Second service, actually a `libsystemd_service_t` (`const struct libsystemd_service*`).
 *
 * @retval <0 `data1` should be started before `data2`.
 * @retval 0  `data1` and `data2` have the same starting_order.
 * @retval >0 `data1` should be started after `data2`.
 *
 * @note Not meant to be called directly - pass it as the `compare_func`
 *       argument to dmlist_sort(), as done in libsystemd_scan().
 */
static int libsystemd_compare_by_starting_order(const void* data1, const void* data2)
{
    const struct libsystemd_service* service1 = (const struct libsystemd_service*)data1;
    const struct libsystemd_service* service2 = (const struct libsystemd_service*)data2;
    return service1->starting_order - service2->starting_order;
}

/**
 * @brief Look up a service in a registry by its unit name
 *
 * @param services  Registry to search, may be NULL.
 * @param unit_name Unit name to search for (e.g. "webserver"), must not be NULL.
 *
 * @return Matching `libsystemd_service_t`, or NULL if @p services is NULL/empty
 *         or no service with that unit name exists.
 *
 * @note The returned pointer is borrowed from the registry - it stays valid
 *       until the registry is replaced/destroyed (e.g. by the next
 *       libsystemd_scan()), and must not be freed by the caller.
 *
 * @par Example
 * @code
 * libsystemd_service_t service = libsystemd_find_service(g_services, "webserver");
 * if (service != NULL) {
 *     // service->exec, service->pid, ... are readable here
 * }
 * @endcode
 */
static libsystemd_service_t libsystemd_find_service(libsystemd_services_t services, const char* unit_name)
{
    if (services == NULL || services->services == NULL)
    {
        return NULL;
    }

    return (libsystemd_service_t)dmlist_find(services->services, unit_name, libsystemd_compare_by_unit_name);
}

/**
 * @brief dmlist_foreach() visitor that folds one dependency name into the running max starting_order
 *
 * Called once per entry of a service's `required`/`after` list (see
 * libsystemd_resolve_service_order_visitor()). Looks the dependency up by name in
 * the full service list and, if found, raises `ctx->max_order` to
 * `dependency->starting_order + 1` when that is higher than what was seen so far.
 *
 * @param data      Dependency unit name, actually a `char*`/`const char*` list entry.
 * @param user_data Fold state, actually a `libsystemd_order_ctx_t*`.
 *
 * @retval true Always - every dependency in the list must be inspected.
 *
 * @note Dependencies that are not found in the service list (e.g. a typo in
 *       "requires"/"after", or a unit that was never scanned) are silently
 *       ignored rather than treated as an error.
 */
static bool libsystemd_dependency_order_visitor(void* data, void* user_data)
{
    const char* dep_name = (const char*)data;
    libsystemd_order_ctx_t* ctx = (libsystemd_order_ctx_t*)user_data;

    libsystemd_service_t dependency = (libsystemd_service_t)dmlist_find(ctx->services, dep_name, libsystemd_compare_by_unit_name);
    if (dependency != NULL)
    {
        int candidate = dependency->starting_order + 1;
        if (candidate > ctx->max_order)
        {
            ctx->max_order = candidate;
        }
    }

    return true;
}

/**
 * @brief dmlist_foreach() visitor that recomputes one service's starting_order for a single relaxation pass
 *
 * Folds over `service->required` and `service->after` (via
 * libsystemd_dependency_order_visitor()) to find the highest
 * `(dependency->starting_order + 1)` among this service's dependencies, and
 * raises `service->starting_order` to that value if it is currently lower.
 *
 * @param data      Service being processed, actually a `libsystemd_service_t`.
 * @param user_data Pass state, actually a `libsystemd_pass_ctx_t*`.
 *
 * @retval true Always - every service in the list must be visited each pass.
 *
 * @note Sets `pass_ctx->changed = true` whenever it actually raises a
 *       starting_order, which is how libsystemd_resolve_starting_order() detects
 *       that fixed point has not yet been reached.
 */
static bool libsystemd_resolve_service_order_visitor(void* data, void* user_data)
{
    libsystemd_service_t service = (libsystemd_service_t)data;
    libsystemd_pass_ctx_t* pass_ctx = (libsystemd_pass_ctx_t*)user_data;

    libsystemd_order_ctx_t order_ctx = { .services = pass_ctx->services, .max_order = 0 };

    if (service->required != NULL)
    {
        dmlist_foreach(service->required, libsystemd_dependency_order_visitor, &order_ctx);
    }
    if (service->after != NULL)
    {
        dmlist_foreach(service->after, libsystemd_dependency_order_visitor, &order_ctx);
    }

    if (order_ctx.max_order > service->starting_order)
    {
        service->starting_order = order_ctx.max_order;
        pass_ctx->changed = true;
    }

    return true;
}

/**
 * @brief Propagate each service's starting_order past every service it requires/comes after
 *
 * Runs Bellman-Ford-style relaxation passes over @p services: a dependency may
 * appear later in the list than its dependent, so a single top-to-bottom pass
 * is not enough to fully propagate the order. Each pass calls
 * libsystemd_resolve_service_order_visitor() for every service; the function
 * stops early as soon as a pass makes no further changes.
 *
 * @param services List of `libsystemd_service_t` entries to reorder in place (starting_order is mutated).
 *
 * @return Nothing. On return, every service's `starting_order` reflects the
 *         longest dependency chain reachable from it (or a partially-resolved
 *         value if a dependency cycle exists - see the note below).
 *
 * @note Bounded to at most `dmlist_size(services)` passes. A dependency cycle
 *       (e.g. two services that "require" each other) simply exhausts every
 *       pass without reaching a fixed point; this is harmless here since the
 *       resulting order is merely used to pick a start sequence, not to
 *       detect/reject invalid configurations.
 *
 * @par Example
 * @code
 * libsystemd_resolve_starting_order(g_services->services);
 * dmlist_sort(g_services->services, libsystemd_compare_by_starting_order);
 * @endcode
 */
static void libsystemd_resolve_starting_order(dmlist_context_t* services)
{
    size_t service_count = dmlist_size(services);

    for (size_t i = 0; i < service_count; i++)
    {
        libsystemd_pass_ctx_t pass_ctx = { .services = services, .changed = false };
        dmlist_foreach(services, libsystemd_resolve_service_order_visitor, &pass_ctx);
        if (!pass_ctx.changed)
        {
            break;
        }
    }
}

/**
 * @brief Actually spawn a service's process, without looking it up by name first
 *
 * Shared by libsystemd_start_service() (which looks the service up by unit name
 * first) and libsystemd_start_service_visitor() (which already has a direct
 * pointer while walking the registry in libsystemd_scan()), so the spawn logic
 * itself lives in exactly one place.
 *
 * Spawns `service->exec` as a module via `Dmod_SpawnModule()`, passing
 * `service->argc`/`service->argv` and, if any stream redirections were parsed,
 * `&service->streams`. On success, records the returned PID in `service->pid`
 * so later libsystemd_stop_service()/libsystemd_status() calls can find the process.
 *
 * @param service Service to start (must not be NULL).
 *
 * @retval 0        The service was spawned successfully; `service->pid` now holds its PID.
 * @retval -EALREADY The service already has a live process associated with it.
 * @retval -ENOSYS  `Dmod_SpawnModule` is not connected on this build/platform (see @ref DMOD_SAL_PROC).
 * @retval <0       Any other negative value is the errno-style error returned by `Dmod_SpawnModule`
 *                   (e.g. -ENOENT if `service->exec` could not be found/loaded as a module).
 *
 * @note `service->pid` is left untouched when spawning fails, so a failed
 *       restart attempt never clobbers the bookkeeping of a still-running
 *       previous instance.
 */
static int libsystemd_start_service_internal(libsystemd_service_t service)
{
    if (service->pid > 0 && dmosi_process_find_by_id((dmosi_process_id_t)service->pid) != NULL)
    {
        return -EALREADY;
    }

    if (!Dmod_IsFunctionConnected((void*)Dmod_SpawnModule))
    {
        return -ENOSYS;
    }

    const Dmod_StreamRedirections_t* streams = (service->streams.Count > 0) ? &service->streams : NULL;
    int spawn_result = Dmod_SpawnModule(service->exec, service->argc, service->argv, streams);
    if (spawn_result < 0)
    {
        return spawn_result;
    }

    service->pid = (Dmod_Pid_t)spawn_result;

    return 0;
}

/**
 * @brief dmlist_foreach() visitor that starts one service, logging (but not propagating) failures
 *
 * Used by libsystemd_scan() to start every service in the freshly sorted
 * registry in order. Failures are logged via DMOD_LOG_WARN() and otherwise
 * ignored, so one misconfigured/missing service does not prevent the rest of
 * the registry from starting.
 *
 * @param data      Service to start, actually a `libsystemd_service_t`.
 * @param user_data Unused (pass NULL).
 *
 * @retval true Always - iteration continues regardless of whether the start succeeded.
 */
static bool libsystemd_start_service_visitor(void* data, void* user_data)
{
    (void)user_data;

    libsystemd_service_t service = (libsystemd_service_t)data;
    int result = libsystemd_start_service_internal(service);
    if (result != 0)
    {
        DMOD_LOG_WARN("Failed to start service '%s' (%d)\n", service->unit_name, result);
    }

    return true;
}

/**
 * @brief Actually stop a service's process, without looking it up by name first
 *
 * Shared by libsystemd_stop_service() (which looks the service up by unit name
 * first) and libsystemd_stop_all_services_visitor() (which already has a direct
 * pointer while walking a registry that is about to be replaced/torn down).
 *
 * Resolves `service->pid` to a live `dmosi_process_t` and kills it via
 * `dmosi_process_kill()`. Always clears `service->pid` back to the "not
 * running" sentinel (-1) once the process is confirmed gone or killed, so a
 * subsequent libsystemd_start_service() call is never blocked by stale state.
 *
 * @param service Service to stop (must not be NULL).
 *
 * @retval 0      The service's process was found and killed successfully.
 * @retval -ESRCH The service had no running process to stop (`pid` was already
 *                 <= 0, or the process it pointed to could no longer be found -
 *                 e.g. it already exited on its own).
 * @retval <0     Any other negative value is the errno-style error returned by
 *                 `dmosi_process_kill()`.
 *
 * @par Example
 * @code
 * libsystemd_service_t service = libsystemd_find_service(g_services, "webserver");
 * if (service != NULL) {
 *     int result = libsystemd_stop_service_internal(service);
 * }
 * @endcode
 */
static int libsystemd_stop_service_internal(libsystemd_service_t service)
{
    if (service->pid <= 0)
    {
        return -ESRCH;
    }

    dmosi_process_t process = dmosi_process_find_by_id((dmosi_process_id_t)service->pid);
    if (process == NULL)
    {
        service->pid = -1;
        return -ESRCH;
    }

    int result = dmosi_process_kill(process, 0);
    if (result != 0)
    {
        return result;
    }

    service->pid = -1;

    return 0;
}

/**
 * @brief dmlist_foreach() visitor that stops one service if it currently looks like it is running
 *
 * Used by libsystemd_stop_all_services() to shut down every service in a
 * registry that is about to be discarded (a rescan via libsystemd_scan(), or
 * module teardown via libsystemd_serviceapi_deinit()). Failures are ignored -
 * there is no registry left afterwards to report status through, and the
 * registry is being destroyed regardless.
 *
 * @param data      Service to stop, actually a `libsystemd_service_t`.
 * @param user_data Unused (pass NULL).
 *
 * @retval true Always - iteration continues regardless of whether the stop succeeded.
 */
static bool libsystemd_stop_all_services_visitor(void* data, void* user_data)
{
    (void)user_data;

    libsystemd_service_t service = (libsystemd_service_t)data;
    if (service->pid > 0)
    {
        libsystemd_stop_service_internal(service);
    }

    return true;
}

/**
 * @brief Stop every currently-running service in a registry
 *
 * @param services Registry to walk, or NULL (no-op).
 *
 * @return Nothing.
 *
 * @note Called before a registry is discarded (see libsystemd_scan() and
 *       libsystemd_serviceapi_deinit()) so that replacing/unloading the service
 *       list never leaves orphaned, untracked processes running.
 *
 * @par Example
 * @code
 * libsystemd_stop_all_services(g_services);
 * libsystemd_destroy_services(g_services);
 * g_services = NULL;
 * @endcode
 */
static void libsystemd_stop_all_services(libsystemd_services_t services)
{
    if (services == NULL || services->services == NULL)
    {
        return;
    }

    dmlist_foreach(services->services, libsystemd_stop_all_services_visitor, NULL);
}

/**
 * @brief Compute a service's current status from its tracked PID
 *
 * Shared by libsystemd_status() and libsystemd_list_visitor() so both report status
 * identically. If the service has never been started (`pid <= 0`), reports
 * `DMOSI_PROCESS_STATE_CREATED` with a PID of 0. If it was started but its
 * process can no longer be found (it exited on its own, without going through
 * libsystemd_stop_service()), reports `DMOSI_PROCESS_STATE_TERMINATED` with the
 * last known PID. Otherwise reports the live process's actual state and PID.
 *
 * @param service    Service to inspect (must not be NULL).
 * @param out_status Status structure to fill in (must not be NULL).
 *
 * @return Nothing; always fills in `*out_status`.
 *
 * @par Example
 * @code
 * libsystemd_service_status_t status;
 * libsystemd_fill_status(service, &status);
 * if (status.state == DMOSI_PROCESS_STATE_RUNNING) { ... }
 * @endcode
 */
static void libsystemd_fill_status(libsystemd_service_t service, libsystemd_service_status_t* out_status)
{
    if (service->pid <= 0)
    {
        out_status->state = DMOSI_PROCESS_STATE_CREATED;
        out_status->pid = 0;
        return;
    }

    dmosi_process_t process = dmosi_process_find_by_id((dmosi_process_id_t)service->pid);
    if (process == NULL)
    {
        out_status->state = DMOSI_PROCESS_STATE_TERMINATED;
        out_status->pid = (dmosi_process_id_t)service->pid;
        return;
    }

    out_status->state = dmosi_process_get_state(process);
    out_status->pid = dmosi_process_get_id(process);
}

/**
 * @brief dmlist_foreach() visitor that reports one service to a caller-supplied libsystemd_visitor_t
 *
 * Builds a `libsystemd_service_info_t` (unit name + status, via
 * libsystemd_fill_status()) for the current service and forwards it to the
 * user's visitor, propagating whatever the visitor returns so libsystemd_list()
 * can be stopped early exactly like dmlist_foreach() itself supports.
 *
 * @param data      Service being visited, actually a `libsystemd_service_t`.
 * @param user_data Fold state, actually a `libsystemd_list_ctx_t*`.
 *
 * @retval true  Continue iterating (the user's visitor returned true).
 * @retval false Stop iterating (the user's visitor returned false).
 */
static bool libsystemd_list_visitor(void* data, void* user_data)
{
    libsystemd_service_t service = (libsystemd_service_t)data;
    libsystemd_list_ctx_t* ctx = (libsystemd_list_ctx_t*)user_data;

    libsystemd_service_info_t info;
    info.unit_name = service->unit_name;
    libsystemd_fill_status(service, &info.status);

    return ctx->visitor(&info, ctx->user_ptr);
}

/**
 * @brief Build a service's argv array from its "exec" and "args" ini keys
 *
 * Allocates `service->argv` as `service->argc + 1` entries: `argv[0]` is an
 * owned copy of @p exec, followed by one owned copy per whitespace-separated
 * token of @p args (consecutive spaces/tabs are treated as a single
 * separator), and a trailing NULL sentinel at `argv[service->argc]`.
 *
 * @param service Service being built; `argc`/`argv` are set on success (must not be NULL).
 * @param exec    Value of the "exec" key, used verbatim as argv[0] (must not be NULL).
 * @param args    Value of the "args" key, or NULL/empty if there are no extra arguments.
 *
 * @retval true  `service->argc`/`service->argv` were populated successfully.
 * @retval false Allocation failed; `service->argv` is left NULL and `service->argc` is left 0.
 *
 * @par Example
 * @code
 * // ini file: exec=dmhttpd
 * //           args=--port 8080
 * libsystemd_build_argv(service, "dmhttpd", "--port 8080");
 * // service->argc == 3
 * // service->argv == { "dmhttpd", "--port", "8080", NULL }
 * @endcode
 */
static bool libsystemd_build_argv(libsystemd_service_t service, const char* exec, const char* args)
{
    int token_count = 0;
    char* args_copy = NULL;

    if (args != NULL && args[0] != '\0')
    {
        args_copy = Dmod_StrDup(args);
        if (args_copy == NULL)
        {
            return false;
        }

        const char* scan = args_copy;
        while (*scan != '\0')
        {
            while (*scan == ' ' || *scan == '\t')
            {
                scan++;
            }
            if (*scan == '\0')
            {
                break;
            }
            token_count++;
            while (*scan != '\0' && *scan != ' ' && *scan != '\t')
            {
                scan++;
            }
        }
    }

    service->argc = 1 + token_count;
    service->argv = Dmod_Malloc(sizeof(char*) * (service->argc + 1));
    if (service->argv == NULL)
    {
        Dmod_Free(args_copy);
        return false;
    }

    service->argv[0] = Dmod_StrDup(exec);
    if (service->argv[0] == NULL)
    {
        Dmod_Free(service->argv);
        service->argv = NULL;
        Dmod_Free(args_copy);
        return false;
    }

    if (args_copy != NULL)
    {
        char* scan = args_copy;
        int index = 1;
        while (*scan != '\0')
        {
            while (*scan == ' ' || *scan == '\t')
            {
                scan++;
            }
            if (*scan == '\0')
            {
                break;
            }
            char* token_start = scan;
            while (*scan != '\0' && *scan != ' ' && *scan != '\t')
            {
                scan++;
            }
            if (*scan != '\0')
            {
                *scan = '\0';
                scan++;
            }
            service->argv[index] = Dmod_StrDup(token_start);
            index++;
        }
        Dmod_Free(args_copy);
    }

    service->argv[service->argc] = NULL;

    return true;
}

/**
 * @brief Build a service's stream redirection table from its "stdin"/"stdout"/"stderr"/"stdlog" ini keys
 *
 * Allocates up to 4 `Dmod_StreamRedirection_t` entries, one for each of
 * "stdin", "stdout", "stderr", "stdlog" that is present in @p ctx, mapping it
 * to `DMOD_STDIN`/`DMOD_STDOUT`/`DMOD_STDERR`/`DMOD_STDLOG` respectively with
 * an owned copy of its path. Keys that are absent are simply skipped -
 * `service->streams.Count` reflects only the keys that were actually present.
 *
 * @param ctx     Parsed ini context to read the keys from (must not be NULL).
 * @param service Service being built; `streams` is set on success (must not be NULL).
 *
 * @retval true  `service->streams` was populated successfully (possibly with `Count == 0`
 *                and `Entries == NULL` if none of the four keys were present).
 * @retval false Allocation of the entries array failed; `service->streams` is left zeroed.
 *
 * @par Example
 * @code
 * // ini file: stdout=/var/log/webserver.log
 * //           stderr=/var/log/webserver.log
 * libsystemd_build_streams(ctx, service);
 * // service->streams.Count == 2
 * @endcode
 */
static bool libsystemd_build_streams(dmini_context_t ctx, libsystemd_service_t service)
{
    struct
    {
        void* handle;
        const char* key;
    } candidates[4] = {
        { DMOD_STDIN,  "stdin"  },
        { DMOD_STDOUT, "stdout" },
        { DMOD_STDERR, "stderr" },
        { DMOD_STDLOG, "stdlog" },
    };

    Dmod_StreamRedirection_t* entries = Dmod_Malloc(sizeof(Dmod_StreamRedirection_t) * 4);
    if (entries == NULL)
    {
        return false;
    }

    size_t count = 0;
    for (size_t i = 0; i < 4; i++)
    {
        const char* path = dmini_get_string(ctx, NULL, candidates[i].key, NULL);
        if (path != NULL)
        {
            entries[count].StdHandle = candidates[i].handle;
            entries[count].Path = Dmod_StrDup(path);
            count++;
        }
    }

    if (count == 0)
    {
        Dmod_Free(entries);
        entries = NULL;
    }

    service->streams.Entries = entries;
    service->streams.Count = count;

    return true;
}

/**
 * @brief Parse a comma/whitespace separated list of unit names into an owned dmlist
 *
 * Used for both the "requires" and "after" ini keys. Always creates a
 * (possibly empty) dmlist on success, even if @p key is absent from @p ctx.
 *
 * @param ctx      Parsed ini context to read the key from (must not be NULL).
 * @param key      Ini key to read (e.g. "requires" or "after").
 * @param out_list Receives the newly created list on success (must not be NULL).
 *
 * @retval true  `*out_list` now holds a valid (possibly empty) dmlist of owned `char*` unit names.
 * @retval false Allocation failed; `*out_list` is left untouched.
 *
 * @par Example
 * @code
 * // ini file: requires=networking
 * dmlist_context_t* required = NULL;
 * libsystemd_parse_name_list(ctx, "requires", &required);
 * // dmlist_size(required) == 1, dmlist_front(required) == "networking"
 * @endcode
 */
static bool libsystemd_parse_name_list(dmini_context_t ctx, const char* key, dmlist_context_t** out_list)
{
    dmlist_context_t* list = dmlist_create(DMOD_MODULE_NAME);
    if (list == NULL)
    {
        return false;
    }

    const char* value = dmini_get_string(ctx, NULL, key, NULL);
    if (value != NULL)
    {
        char* value_copy = Dmod_StrDup(value);
        if (value_copy == NULL)
        {
            dmlist_destroy(list);
            return false;
        }

        char* scan = value_copy;
        while (*scan != '\0')
        {
            while (*scan == ' ' || *scan == '\t' || *scan == ',')
            {
                scan++;
            }
            if (*scan == '\0')
            {
                break;
            }
            char* token_start = scan;
            while (*scan != '\0' && *scan != ' ' && *scan != '\t' && *scan != ',')
            {
                scan++;
            }
            if (*scan != '\0')
            {
                *scan = '\0';
                scan++;
            }

            char* name_copy = Dmod_StrDup(token_start);
            if (name_copy != NULL)
            {
                dmlist_push_back(list, name_copy);
            }
        }

        Dmod_Free(value_copy);
    }

    *out_list = list;

    return true;
}

/**
 * @brief Check whether a file name ends in the ".ini" extension (case-sensitive)
 *
 * @param file_name Bare file name to check (e.g. "webserver.ini"), must not be NULL.
 *
 * @retval true  @p file_name is longer than ".ini" and ends with it.
 * @retval false Otherwise (including names that are exactly ".ini", which would
 *                yield an empty unit name if accepted).
 *
 * @par Example
 * @code
 * libsystemd_has_ini_extension("webserver.ini"); // true
 * libsystemd_has_ini_extension("README.md");     // false
 * @endcode
 */
static bool libsystemd_has_ini_extension(const char* file_name)
{
    size_t len = strlen(file_name);
    return (len > 4) && (strcmp(file_name + len - 4, ".ini") == 0);
}

/**
 * @brief Derive a unit name from a ".ini" file name by stripping the extension
 *
 * @param file_name Bare file name to derive from (e.g. "webserver.ini"); must
 *                    already satisfy libsystemd_has_ini_extension().
 *
 * @return Newly heap-allocated, NUL-terminated unit name (e.g. "webserver"),
 *         owned by the caller (free with Dmod_Free()), or NULL if allocation failed.
 *
 * @par Example
 * @code
 * char* unit_name = libsystemd_make_unit_name("webserver.ini");
 * // unit_name == "webserver"
 * Dmod_Free(unit_name);
 * @endcode
 */
static char* libsystemd_make_unit_name(const char* file_name)
{
    size_t len = strlen(file_name) - 4; /* strip trailing ".ini" */

    char* unit_name = Dmod_Malloc(len + 1);
    if (unit_name == NULL)
    {
        return NULL;
    }

    memcpy(unit_name, file_name, len);
    unit_name[len] = '\0';

    return unit_name;
}

/**
 * @brief Join a directory path and a file name with a single '/' separator
 *
 * @param dir_path  Directory path, with or without a trailing '/' (must not be NULL).
 * @param file_name File name to append (must not be NULL).
 *
 * @return Newly heap-allocated, NUL-terminated joined path, owned by the
 *         caller (free with Dmod_Free()), or NULL if allocation failed.
 *
 * @par Example
 * @code
 * char* path = libsystemd_join_path("/etc/services", "webserver.ini");
 * // path == "/etc/services/webserver.ini"
 * Dmod_Free(path);
 * @endcode
 */
static char* libsystemd_join_path(const char* dir_path, const char* file_name)
{
    size_t dir_len = strlen(dir_path);
    bool needs_separator = (dir_len > 0) && (dir_path[dir_len - 1] != '/');
    size_t file_len = strlen(file_name);
    size_t total_len = dir_len + (needs_separator ? 1 : 0) + file_len;

    char* joined = Dmod_Malloc(total_len + 1);
    if (joined == NULL)
    {
        return NULL;
    }

    memcpy(joined, dir_path, dir_len);
    size_t offset = dir_len;
    if (needs_separator)
    {
        joined[offset++] = '/';
    }
    memcpy(joined + offset, file_name, file_len);
    joined[offset + file_len] = '\0';

    return joined;
}

/**
 * @brief Classification of a ".ini" unit file name with respect to systemd-style `@` templating
 */
typedef enum
{
    LIBSYSTEMD_UNIT_PLAIN,     //!< No '@' in the name (e.g. "webserver.ini") - parsed as-is, no specifier substitution.
    LIBSYSTEMD_UNIT_TEMPLATE,  //!< '@' immediately followed by ".ini" (e.g. "getty@.ini") - never parsed/started on its own.
    LIBSYSTEMD_UNIT_INSTANCE,  //!< '@' followed by a non-empty instance (e.g. "getty@tty1.ini") - merged with its template, if any.
} libsystemd_unit_kind_t;

/**
 * @brief Split a ".ini" unit file name into its template prefix/instance, systemd-`@`-style
 *
 * Looks for the first `@` before the ".ini" suffix. `"foo.ini"` (no `@`) is
 * ::LIBSYSTEMD_UNIT_PLAIN. `"foo@.ini"` (empty instance) is
 * ::LIBSYSTEMD_UNIT_TEMPLATE - the bare template, never started on its own.
 * `"foo@bar.ini"` is ::LIBSYSTEMD_UNIT_INSTANCE with prefix `"foo"` and
 * instance `"bar"`.
 *
 * @param file_name   Bare file name to classify; must already satisfy
 *                      libsystemd_has_ini_extension().
 * @param out_prefix  Receives an owned copy of the part before `@` for
 *                      ::LIBSYSTEMD_UNIT_TEMPLATE/::LIBSYSTEMD_UNIT_INSTANCE,
 *                      or NULL for ::LIBSYSTEMD_UNIT_PLAIN (must not be NULL).
 * @param out_instance Receives an owned copy of the part between `@` and
 *                      ".ini" for ::LIBSYSTEMD_UNIT_INSTANCE, or NULL otherwise
 *                      (must not be NULL).
 *
 * @return The file's ::libsystemd_unit_kind_t. On allocation failure, falls
 *         back to ::LIBSYSTEMD_UNIT_PLAIN with both output pointers NULL,
 *         which simply causes the file to be parsed without template
 *         expansion rather than aborting the whole directory scan.
 *
 * @par Example
 * @code
 * char* prefix = NULL;
 * char* instance = NULL;
 * libsystemd_classify_unit_file("getty@tty1.ini", &prefix, &instance);
 * // kind == LIBSYSTEMD_UNIT_INSTANCE, prefix == "getty", instance == "tty1"
 * @endcode
 */
static libsystemd_unit_kind_t libsystemd_classify_unit_file(const char* file_name, char** out_prefix, char** out_instance)
{
    *out_prefix = NULL;
    *out_instance = NULL;

    size_t base_len = strlen(file_name) - 4; /* strip trailing ".ini" */

    const char* at = NULL;
    for (size_t i = 0; i < base_len; i++)
    {
        if (file_name[i] == '@')
        {
            at = file_name + i;
            break;
        }
    }
    if (at == NULL)
    {
        return LIBSYSTEMD_UNIT_PLAIN;
    }

    size_t prefix_len = (size_t)(at - file_name);
    size_t instance_len = base_len - prefix_len - 1;

    char* prefix = Dmod_Malloc(prefix_len + 1);
    if (prefix == NULL)
    {
        return LIBSYSTEMD_UNIT_PLAIN;
    }
    memcpy(prefix, file_name, prefix_len);
    prefix[prefix_len] = '\0';

    if (instance_len == 0)
    {
        *out_prefix = prefix;
        return LIBSYSTEMD_UNIT_TEMPLATE;
    }

    char* instance = Dmod_Malloc(instance_len + 1);
    if (instance == NULL)
    {
        Dmod_Free(prefix);
        return LIBSYSTEMD_UNIT_PLAIN;
    }
    memcpy(instance, at + 1, instance_len);
    instance[instance_len] = '\0';

    *out_prefix = prefix;
    *out_instance = instance;
    return LIBSYSTEMD_UNIT_INSTANCE;
}

/**
 * @brief Build the file path of a template's own unit file ("<prefix>@.ini") inside a directory
 *
 * @param dir_path Directory the instance file was found in (must not be NULL).
 * @param prefix   Template prefix, as produced by libsystemd_classify_unit_file() (must not be NULL).
 *
 * @return Newly heap-allocated path (e.g. "/etc/units/getty@.ini"), owned by
 *         the caller (free with Dmod_Free()), or NULL if allocation failed.
 */
static char* libsystemd_build_template_path(const char* dir_path, const char* prefix)
{
    size_t prefix_len = strlen(prefix);

    char* file_name = Dmod_Malloc(prefix_len + 5 /* "@.ini" */ + 1);
    if (file_name == NULL)
    {
        return NULL;
    }
    memcpy(file_name, prefix, prefix_len);
    memcpy(file_name + prefix_len, "@.ini", 5);
    file_name[prefix_len + 5] = '\0';

    char* path = libsystemd_join_path(dir_path, file_name);
    Dmod_Free(file_name);

    return path;
}

/**
 * @brief Build an instantiated unit's full name ("<prefix>@<instance>") from its parts
 *
 * @param prefix   Template prefix (must not be NULL).
 * @param instance Instance name (must not be NULL).
 *
 * @return Newly heap-allocated, NUL-terminated unit name, owned by the caller
 *         (free with Dmod_Free()), or NULL if allocation failed.
 */
static char* libsystemd_build_unit_name(const char* prefix, const char* instance)
{
    size_t prefix_len = strlen(prefix);
    size_t instance_len = strlen(instance);

    char* unit_name = Dmod_Malloc(prefix_len + 1 + instance_len + 1);
    if (unit_name == NULL)
    {
        return NULL;
    }

    memcpy(unit_name, prefix, prefix_len);
    unit_name[prefix_len] = '@';
    memcpy(unit_name + prefix_len + 1, instance, instance_len);
    unit_name[prefix_len + 1 + instance_len] = '\0';

    return unit_name;
}

/**
 * @brief Expand systemd-style `%`-specifiers in a single string
 *
 * Recognized specifiers: `%i`/`%I` (instance name), `%p` (template prefix),
 * `%n` (full instantiated unit name, "<prefix>@<instance>"), `%%` (a literal
 * `%`). An unrecognized `%<char>` sequence (or a trailing `%` at the end of
 * the string) is copied through verbatim, unexpanded.
 *
 * @param value    String to expand (must not be NULL).
 * @param prefix   Template prefix, substituted for `%p` (must not be NULL).
 * @param instance Instance name, substituted for `%i`/`%I` (must not be NULL).
 * @param unit_name Full "<prefix>@<instance>" unit name, substituted for `%n` (must not be NULL).
 *
 * @return Newly heap-allocated expanded string, owned by the caller (free
 *         with Dmod_Free()), or NULL if allocation failed.
 *
 * @par Example
 * @code
 * char* expanded = libsystemd_substitute_specifiers("--tty %i", "getty", "tty1", "getty@tty1");
 * // expanded == "--tty tty1"
 * @endcode
 */
static char* libsystemd_substitute_specifiers(const char* value, const char* prefix, const char* instance, const char* unit_name)
{
    size_t prefix_len = strlen(prefix);
    size_t instance_len = strlen(instance);
    size_t unit_name_len = strlen(unit_name);

    size_t out_len = 0;
    for (const char* scan = value; *scan != '\0'; )
    {
        if (scan[0] == '%' && scan[1] != '\0')
        {
            switch (scan[1])
            {
                case 'i': case 'I': out_len += instance_len; scan += 2; continue;
                case 'p':           out_len += prefix_len;   scan += 2; continue;
                case 'n':           out_len += unit_name_len; scan += 2; continue;
                case '%':           out_len += 1;             scan += 2; continue;
                default: break; /* unknown specifier: copied through as-is below */
            }
        }
        out_len += 1;
        scan += 1;
    }

    char* result = Dmod_Malloc(out_len + 1);
    if (result == NULL)
    {
        return NULL;
    }

    char* dst = result;
    for (const char* scan = value; *scan != '\0'; )
    {
        if (scan[0] == '%' && scan[1] != '\0')
        {
            const char* replacement = NULL;
            size_t replacement_len = 0;
            switch (scan[1])
            {
                case 'i': case 'I': replacement = instance;   replacement_len = instance_len;   break;
                case 'p':           replacement = prefix;     replacement_len = prefix_len;     break;
                case 'n':           replacement = unit_name;  replacement_len = unit_name_len;  break;
                case '%':           replacement = "%";        replacement_len = 1;               break;
                default: break;
            }

            if (replacement != NULL)
            {
                memcpy(dst, replacement, replacement_len);
                dst += replacement_len;
                scan += 2;
                continue;
            }
        }
        *dst++ = *scan++;
    }
    *dst = '\0';

    return result;
}

/**
 * @brief Expand `%`-specifiers in every key of an ini context's global section, in place
 *
 * Walks every key currently in @p ctx's global section (via
 * dmini_key_count()/dmini_key_name()) and, for any value containing a `%`,
 * replaces it with its libsystemd_substitute_specifiers() expansion
 * (dmini_set_string() updates the existing key in place - see
 * `set_pair_in_section()` in dmini - so this does not disturb key order/count
 * while iterating by index).
 *
 * @param ctx      Ini context to expand in place (must not be NULL).
 * @param prefix   Template prefix, forwarded to libsystemd_substitute_specifiers().
 * @param instance Instance name, forwarded to libsystemd_substitute_specifiers().
 * @param unit_name Full unit name, forwarded to libsystemd_substitute_specifiers().
 *
 * @retval true  Every key was expanded successfully.
 * @retval false Allocation failed part-way through; @p ctx is left with
 *                whatever subset of keys had already been expanded.
 */
static bool libsystemd_apply_specifiers(dmini_context_t ctx, const char* prefix, const char* instance, const char* unit_name)
{
    int count = dmini_key_count(ctx, NULL);

    for (int i = 0; i < count; i++)
    {
        const char* key = dmini_key_name(ctx, NULL, i);
        if (key == NULL)
        {
            continue;
        }

        const char* value = dmini_get_string(ctx, NULL, key, NULL);
        if (value == NULL || strchr(value, '%') == NULL)
        {
            continue;
        }

        char* expanded = libsystemd_substitute_specifiers(value, prefix, instance, unit_name);
        if (expanded == NULL)
        {
            return false;
        }

        dmini_set_string(ctx, NULL, key, expanded);
        Dmod_Free(expanded);
    }

    return true;
}

/**
 * @brief Build a fresh `libsystemd_service_t` from an already-parsed ini context
 *
 * Shared tail end of both libsystemd_parse_unit_internal() (file-backed
 * parsing) and libsystemd_instantiate_from_template() (on-demand template
 * instantiation with no file of its own) - everything from here on only
 * cares about the fully-resolved `dmini_context_t`, not where it came from.
 *
 * @param ctx     Fully-parsed (and, for a template instance, already
 *                  specifier-expanded) ini context to read from (must not be NULL).
 * @param service Receives the newly allocated service on success (must not
 *                  be NULL). `unit_name` is left NULL - callers set it themselves,
 *                  since neither of this function's two callers has one single
 *                  obvious source for it (a file name vs. a synthesized "<prefix>@<instance>").
 *
 * @retval 0       `*service` now points to a fully populated service (unit_name is NULL,
 *                   starting_order is 0, pid is -1).
 * @retval -EINVAL @p ctx has no "exec" key.
 * @retval -ENOMEM Allocation failed at some point; nothing is leaked.
 */
static int libsystemd_build_service_from_ctx(dmini_context_t ctx, libsystemd_service_t* service)
{
    const char* exec = dmini_get_string(ctx, NULL, "exec", NULL);
    if (exec == NULL)
    {
        return -EINVAL;
    }

    libsystemd_service_t new_service = Dmod_Malloc(sizeof(struct libsystemd_service));
    if (new_service == NULL)
    {
        return -ENOMEM;
    }

    new_service->unit_name = NULL;
    new_service->exec = NULL;
    new_service->argc = 0;
    new_service->argv = NULL;
    new_service->streams.Entries = NULL;
    new_service->streams.Count = 0;
    new_service->starting_order = 0;
    new_service->required = NULL;
    new_service->after = NULL;
    new_service->pid = -1;

    const char* args = dmini_get_string(ctx, NULL, "args", NULL);

    bool ok = (new_service->exec = Dmod_StrDup(exec)) != NULL;
    ok = ok && libsystemd_build_argv(new_service, exec, args);
    ok = ok && libsystemd_build_streams(ctx, new_service);
    ok = ok && libsystemd_parse_name_list(ctx, "requires", &new_service->required);
    ok = ok && libsystemd_parse_name_list(ctx, "after", &new_service->after);

    if (!ok)
    {
        libsystemd_destroy_service(new_service);
        return -ENOMEM;
    }

    *service = new_service;

    return 0;
}

/**
 * @brief Split a unit *name* (not a file name - no ".ini") into its `@` template prefix/instance
 *
 * Used by libsystemd_instantiate_service_on_demand() to decide whether a
 * unit name that was not found in the registry could still be resolved as a
 * template instance (e.g. "getty@tty1" -> prefix "getty", instance "tty1").
 * Unlike libsystemd_classify_unit_file(), a name with an `@` but an empty
 * prefix or instance (e.g. "@foo", "foo@") is rejected outright - there is no
 * such thing as "starting" a bare template.
 *
 * @param unit_name   Unit name to split (must not be NULL).
 * @param out_prefix  Receives an owned copy of the part before `@` on success (must not be NULL).
 * @param out_instance Receives an owned copy of the part after `@` on success (must not be NULL).
 *
 * @retval true  @p unit_name is "<prefix>@<instance>" with both non-empty; `*out_prefix`/`*out_instance` are set.
 * @retval false @p unit_name has no `@`, an empty prefix, an empty instance, or allocation failed;
 *                both output pointers are left NULL.
 */
static bool libsystemd_split_instance_name(const char* unit_name, char** out_prefix, char** out_instance)
{
    *out_prefix = NULL;
    *out_instance = NULL;

    size_t len = strlen(unit_name);

    const char* at = NULL;
    for (size_t i = 0; i < len; i++)
    {
        if (unit_name[i] == '@')
        {
            at = unit_name + i;
            break;
        }
    }
    if (at == NULL)
    {
        return false;
    }

    size_t prefix_len = (size_t)(at - unit_name);
    size_t instance_len = len - prefix_len - 1;
    if (prefix_len == 0 || instance_len == 0)
    {
        return false;
    }

    char* prefix = Dmod_Malloc(prefix_len + 1);
    if (prefix == NULL)
    {
        return false;
    }
    memcpy(prefix, unit_name, prefix_len);
    prefix[prefix_len] = '\0';

    char* instance = Dmod_Malloc(instance_len + 1);
    if (instance == NULL)
    {
        Dmod_Free(prefix);
        return false;
    }
    memcpy(instance, at + 1, instance_len);
    instance[instance_len] = '\0';

    *out_prefix = prefix;
    *out_instance = instance;
    return true;
}

/**
 * @brief Synthesize a service purely from a template, with no on-disk instance file
 *
 * Parses `<units_dir>/<prefix>@.ini` and expands `%i`/`%I`/`%p`/`%n`/`%%` for
 * the given @p instance, exactly like libsystemd_parse_dir() would for an
 * on-disk instance file - the difference is that no such file needs to
 * exist. This is the "systemctl start foo@bar" analog: unlike the directory
 * scan (which only starts instances that already have their own `*.ini`
 * file), this lets any instance name be started as long as its template
 * exists.
 *
 * @param units_dir Directory to look for "<prefix>@.ini" in (must not be NULL).
 * @param prefix    Template prefix, e.g. "getty" (must not be NULL).
 * @param instance  Instance name, e.g. "tty1" (must not be NULL).
 * @param service   Receives the newly allocated service on success, with
 *                    `unit_name` already set to "<prefix>@<instance>" (must not be NULL).
 *
 * @retval 0       `*service` is fully populated and owns its `unit_name`.
 * @retval -ENOMEM Allocation failed.
 * @retval -EINVAL The resolved template has no "exec" key.
 * @retval <0      Any other negative value is a `DMINI_ERR_*` code forwarded
 *                   from `dmini_parse_file` on the template (e.g.
 *                   `DMINI_ERR_FILE` if "<prefix>@.ini" does not exist).
 */
static int libsystemd_instantiate_from_template(const char* units_dir, const char* prefix, const char* instance, libsystemd_service_t* service)
{
    char* template_path = libsystemd_build_template_path(units_dir, prefix);
    if (template_path == NULL)
    {
        return -ENOMEM;
    }

    dmini_context_t ctx = dmini_create();
    if (ctx == NULL)
    {
        Dmod_Free(template_path);
        return -ENOMEM;
    }

    int result = dmini_parse_file(ctx, template_path);
    Dmod_Free(template_path);
    if (result != DMINI_OK)
    {
        dmini_destroy(ctx);
        return result;
    }

    char* unit_name = libsystemd_build_unit_name(prefix, instance);
    if (unit_name == NULL)
    {
        dmini_destroy(ctx);
        return -ENOMEM;
    }

    if (!libsystemd_apply_specifiers(ctx, prefix, instance, unit_name))
    {
        dmini_destroy(ctx);
        Dmod_Free(unit_name);
        return -ENOMEM;
    }

    libsystemd_service_t new_service = NULL;
    result = libsystemd_build_service_from_ctx(ctx, &new_service);
    dmini_destroy(ctx);

    if (result != 0)
    {
        Dmod_Free(unit_name);
        return result;
    }

    new_service->unit_name = unit_name;
    *service = new_service;

    return 0;
}

/**
 * @brief Resolve a unit name that was not found in @ref g_services as an on-demand template instance
 *
 * Called from libsystemd_start_service() when @p unit_name isn't already
 * registered. If @p unit_name is "<prefix>@<instance>"-shaped
 * (libsystemd_split_instance_name()) and a units directory is known (@ref
 * g_units_dir, set by the last successful libsystemd_scan()), tries to
 * instantiate it from "<prefix>@.ini" (libsystemd_instantiate_from_template())
 * and, on success, adds it to @ref g_services so it behaves exactly like any
 * other unit from then on (found by libsystemd_status()/libsystemd_stop_service()/
 * libsystemd_list(), and not re-instantiated on a later libsystemd_start_service() call).
 *
 * @param unit_name Unit name that was not found by libsystemd_find_service() (must not be NULL).
 *
 * @return The newly instantiated and registered service, or NULL if
 *         @p unit_name is not template-shaped, no units directory is known
 *         yet, no matching template exists, or allocation failed.
 */
static libsystemd_service_t libsystemd_instantiate_service_on_demand(const char* unit_name)
{
    if (g_units_dir == NULL || g_services == NULL || g_services->services == NULL)
    {
        return NULL;
    }

    char* prefix = NULL;
    char* instance = NULL;
    if (!libsystemd_split_instance_name(unit_name, &prefix, &instance))
    {
        return NULL;
    }

    libsystemd_service_t service = NULL;
    int result = libsystemd_instantiate_from_template(g_units_dir, prefix, instance, &service);

    Dmod_Free(prefix);
    Dmod_Free(instance);

    if (result != 0)
    {
        return NULL;
    }

    if (!dmlist_push_back(g_services->services, service))
    {
        libsystemd_destroy_service(service);
        return NULL;
    }

    return service;
}

/**
 * @brief Allocate and initialize the global service registry (@ref g_services)
 *
 * Idempotent: if @ref g_services is already allocated, returns 0 immediately
 * without touching it. Called automatically by dmod_init() when the module is
 * loaded, and defensively by libsystemd_scan() in case it ever runs before
 * dmod_init() (e.g. from a test harness that calls API functions directly).
 *
 * @return 0 on success (including the "already initialized" case), or a
 *         negative error code on failure.
 *
 * @retval 0       @ref g_services is now a valid, usable (possibly still empty) registry.
 * @retval -ENOMEM Allocation of the registry or its underlying dmlist failed;
 *                  @ref g_services is left NULL.
 *
 * @par Example
 * @code
 * int result = libsystemd_serviceapi_init();
 * if (result != 0) {
 *     // g_services is still NULL, nothing else in this file will work
 * }
 * @endcode
 */
static int libsystemd_serviceapi_init(void)
{
    if (g_services != NULL)
    {
        return 0;
    }

    libsystemd_services_t services = Dmod_Malloc(sizeof(struct libsystemd_services));
    if (services == NULL)
    {
        return -ENOMEM;
    }

    services->services = dmlist_create(DMOD_MODULE_NAME);
    if (services->services == NULL)
    {
        Dmod_Free(services);
        return -ENOMEM;
    }

    g_services = services;

    return 0;
}

/**
 * @brief Stop every running service and release the global service registry (@ref g_services)
 *
 * Stops every currently-running service (via libsystemd_stop_all_services()) so
 * that tearing down the registry never leaves orphaned processes behind,
 * destroys every service and the registry itself (via
 * libsystemd_destroy_services()), and resets @ref g_services back to NULL. Called
 * automatically by dmod_deinit() when the module is unloaded.
 *
 * @return Nothing.
 *
 * @note Safe to call when @ref g_services is already NULL (both helper calls
 *       tolerate a NULL registry).
 *
 * @par Example
 * @code
 * libsystemd_serviceapi_deinit();
 * // g_services is now NULL again; libsystemd_start_service() etc. will report -ENOENT
 * @endcode
 */
static void libsystemd_serviceapi_deinit(void)
{
    libsystemd_stop_all_services(g_services);
    libsystemd_destroy_services(g_services);
    g_services = NULL;

    Dmod_Free(g_units_dir);
    g_units_dir = NULL;
}

/**
 * @brief Module lifecycle hook: prepares the global service registry when this module is loaded
 *
 * Recognized by name by the DMOD loader (see `Dmod_Init_t` in dmod_types.h) -
 * every module may optionally define a function with this exact name and
 * signature, and the loader calls it once right after loading the module and
 * before running its `main()`. This module uses it to allocate @ref g_services
 * via libsystemd_serviceapi_init(), so the registry is always ready before any
 * of libsystemd_scan()/libsystemd_start_service()/libsystemd_stop_service()/
 * libsystemd_status()/libsystemd_list() can possibly be called.
 *
 * @param Config Module configuration blob passed by the loader; unused by this module.
 *
 * @retval 0       Initialization succeeded.
 * @retval -ENOMEM Initialization failed (see libsystemd_serviceapi_init()); the
 *                  loader is expected to treat this as a failed module load.
 *
 * @note Not meant to be called directly by application code - it is invoked
 *       automatically by the DMOD loader.
 */
int dmod_init(const Dmod_Config_t* Config)
{
    (void)Config;

    return libsystemd_serviceapi_init();
}

/**
 * @brief Module lifecycle hook: stops every running service and releases the global service registry when this module is unloaded
 *
 * Recognized by name by the DMOD loader (see `Dmod_Deinit_t` in
 * dmod_types.h) - called once when the module is being unloaded. Delegates to
 * libsystemd_serviceapi_deinit(), so no process spawned by this module instance
 * is left running untracked after unload.
 *
 * @return 0 always (this hook has no failure mode of its own).
 *
 * @note Not meant to be called directly by application code - it is invoked
 *       automatically by the DMOD loader.
 */
int dmod_deinit(void)
{
    libsystemd_serviceapi_deinit();

    return 0;
}

/**
 * @brief Start a previously scanned service by unit name, instantiating it from a template on demand if needed
 *
 * Looks @p unit_name up in the global registry (@ref g_services, populated by
 * libsystemd_scan()) and, if found, spawns it via libsystemd_start_service_internal().
 *
 * If @p unit_name is not already known, but is "<prefix>@<instance>"-shaped
 * and a matching "<prefix>@.ini" template exists in the last-scanned units
 * directory, it is instantiated on the fly
 * (libsystemd_instantiate_service_on_demand()) - this is the
 * `systemctl start foo@bar` analog: an instance does not need its own `*.ini`
 * file to be started, only its template does. The newly instantiated service
 * is added to the registry, so a later libsystemd_status()/
 * libsystemd_stop_service()/libsystemd_list() sees it exactly like any unit
 * that was already on disk at the last libsystemd_scan().
 *
 * @param unit_name Unit name to start (e.g. "webserver", or "getty@tty1" for
 *                    a template instance), as derived by libsystemd_parse_dir()
 *                    from the ini file name, or synthesized as "<prefix>@<instance>".
 *
 * @retval 0         The service was found (or instantiated) and spawned successfully.
 * @retval -EINVAL   @p unit_name was NULL.
 * @retval -ENOENT   No service with that unit name exists in the registry, and it
 *                     could not be instantiated from a template either (not
 *                     "<prefix>@<instance>"-shaped, no units directory known yet,
 *                     no matching template, or allocation failed).
 * @retval -EALREADY The service already has a live process associated with it.
 * @retval -ENOSYS   Module spawning is not available on this build/platform.
 * @retval <0        Any other negative value forwarded from `Dmod_SpawnModule`.
 *
 * @par Example
 * @code
 * libsystemd_scan("/etc/services"); // only has getty@.ini, no getty@tty1.ini
 * int result = libsystemd_start_service("getty@tty1"); // instantiated on the fly
 * if (result != 0) {
 *     Dmod_Printf("failed to start getty@tty1: %d\n", result);
 * }
 * @endcode
 */
dmod_libsystemd_api_declaration(1.0, int, _start_service, ( const char* unit_name ))
{
    if (unit_name == NULL)
    {
        return -EINVAL;
    }

    libsystemd_service_t service = libsystemd_find_service(g_services, unit_name);
    if (service == NULL)
    {
        service = libsystemd_instantiate_service_on_demand(unit_name);
    }
    if (service == NULL)
    {
        return -ENOENT;
    }

    return libsystemd_start_service_internal(service);
}

/**
 * @brief Stop a previously started service by unit name
 *
 * Looks @p unit_name up in the global registry (@ref g_services) and, if
 * found, kills its tracked process via libsystemd_stop_service_internal().
 *
 * @param unit_name Unit name to stop (e.g. "webserver").
 *
 * @retval 0       The service's process was found and killed successfully.
 * @retval -EINVAL @p unit_name was NULL.
 * @retval -ENOENT No service with that unit name exists in the registry.
 * @retval -ESRCH  The service exists but has no running process to stop.
 * @retval <0      Any other negative value forwarded from `dmosi_process_kill`.
 *
 * @par Example
 * @code
 * int result = libsystemd_stop_service("webserver");
 * if (result == -ESRCH) {
 *     // webserver was not running
 * }
 * @endcode
 */
dmod_libsystemd_api_declaration(1.0, int, _stop_service, ( const char* unit_name ))
{
    if (unit_name == NULL)
    {
        return -EINVAL;
    }

    libsystemd_service_t service = libsystemd_find_service(g_services, unit_name);
    if (service == NULL)
    {
        return -ENOENT;
    }

    return libsystemd_stop_service_internal(service);
}

/**
 * @brief Query the current status of a previously scanned service by unit name
 *
 * Looks @p unit_name up in the global registry (@ref g_services) and, if
 * found, fills in @p out_status via libsystemd_fill_status().
 *
 * @param unit_name  Unit name to query (e.g. "webserver").
 * @param out_status Receives the service's current process state and PID on success.
 *
 * @retval 0       @p out_status now reflects the service's current status.
 * @retval -EINVAL @p unit_name or @p out_status was NULL.
 * @retval -ENOENT No service with that unit name exists in the registry.
 *
 * @par Example
 * @code
 * libsystemd_service_status_t status;
 * if (libsystemd_status("webserver", &status) == 0) {
 *     Dmod_Printf("webserver pid=%u state=%d\n", status.pid, status.state);
 * }
 * @endcode
 */
dmod_libsystemd_api_declaration(1.0, int, _status, ( const char* unit_name, libsystemd_service_status_t* out_status ))
{
    if (unit_name == NULL || out_status == NULL)
    {
        return -EINVAL;
    }

    libsystemd_service_t service = libsystemd_find_service(g_services, unit_name);
    if (service == NULL)
    {
        return -ENOENT;
    }

    libsystemd_fill_status(service, out_status);

    return 0;
}

/**
 * @brief Visit every service currently in the global registry
 *
 * Walks the global registry (@ref g_services) in its current order (the
 * dependency-resolved start order after libsystemd_scan(), unless the registry
 * is empty/uninitialized) and calls @p visitor once per service with a
 * `libsystemd_service_info_t` describing it, until either the registry is
 * exhausted or @p visitor returns false.
 *
 * @param visitor  Callback invoked once per service; return false from it to stop early.
 * @param user_ptr Opaque pointer forwarded verbatim to every @p visitor call.
 *
 * @retval 0       Iteration completed (whether it ran to the end or was
 *                   stopped early by the visitor, and even if the registry
 *                   was empty/uninitialized - there is simply nothing to visit).
 * @retval -EINVAL @p visitor was NULL.
 *
 * @par Example
 * @code
 * static bool print_service(const libsystemd_service_info_t* info, void* user_ptr)
 * {
 *     Dmod_Printf("%s: state=%d\n", info->unit_name, info->status.state);
 *     return true; // keep going
 * }
 *
 * libsystemd_list(print_service, NULL);
 * @endcode
 */
dmod_libsystemd_api_declaration(1.0, int, _list, (libsystemd_visitor_t visitor, void* user_ptr))
{
    if (visitor == NULL)
    {
        return -EINVAL;
    }

    if (g_services == NULL || g_services->services == NULL)
    {
        return 0;
    }

    libsystemd_list_ctx_t ctx = { .visitor = visitor, .user_ptr = user_ptr };
    dmlist_foreach(g_services->services, libsystemd_list_visitor, &ctx);

    return 0;
}

/**
 * @brief Scan a directory of ".ini" unit files, replace the global registry, and start every service
 *
 * The full pipeline described for this module:
 * 1. Ensures the registry exists (libsystemd_serviceapi_init(), idempotent).
 * 2. Parses every "*.ini" file in @p path into a fresh registry (libsystemd_parse_dir()).
 * 3. Resolves each service's starting_order from its "requires"/"after"
 *    dependencies (libsystemd_resolve_starting_order()).
 * 4. Sorts the fresh registry by starting_order (dmlist_sort() with
 *    libsystemd_compare_by_starting_order()).
 * 5. Stops every service in the *previous* registry and destroys it
 *    (libsystemd_stop_all_services() + libsystemd_destroy_services()), so a rescan
 *    never leaves the old generation's processes running untracked.
 * 6. Installs the fresh, sorted registry as the new @ref g_services, and
 *    remembers @p path as @ref g_units_dir for later on-demand template
 *    instantiation (see libsystemd_start_service()) - best-effort, a failure
 *    to remember it does not fail the scan.
 * 7. Starts every service in the new registry, in order
 *    (libsystemd_start_service_visitor() via dmlist_foreach()) - failures for
 *    individual services are logged and do not abort the scan.
 *
 * @param path Directory to scan for "*.ini" unit files (e.g. "/etc/services").
 *
 * @retval 0       The directory was scanned and the registry replaced; individual
 *                   service start failures are logged, not reflected in the return value.
 * @retval -EINVAL @p path was NULL.
 * @retval -ENOMEM Registry (re-)initialization or parsing failed to allocate memory.
 * @retval -ENOENT @p path does not exist / cannot be opened.
 * @retval <0      Any other negative value forwarded from libsystemd_parse_dir().
 *
 * @note Calling this again later is a full reload: unit files that disappeared
 *       since the last scan are dropped, and every service - even ones whose
 *       ini file did not change - is stopped and restarted.
 *
 * @par Example
 * @code
 * int result = libsystemd_scan("/etc/services");
 * if (result != 0) {
 *     Dmod_Printf("service scan failed: %d\n", result);
 * }
 * @endcode
 */
dmod_libsystemd_api_declaration(1.0, int, _scan, (const char* path))
{
    if (path == NULL)
    {
        return -EINVAL;
    }

    int result = libsystemd_serviceapi_init();
    if (result != 0)
    {
        return result;
    }

    libsystemd_services_t parsed = NULL;
    result = libsystemd_parse_dir(path, &parsed);
    if (result != 0)
    {
        return result;
    }

    libsystemd_resolve_starting_order(parsed->services);
    dmlist_sort(parsed->services, libsystemd_compare_by_starting_order);

    libsystemd_stop_all_services(g_services);
    libsystemd_destroy_services(g_services);
    g_services = parsed;

    char* units_dir = Dmod_StrDup(path);
    if (units_dir != NULL)
    {
        Dmod_Free(g_units_dir);
        g_units_dir = units_dir;
    }

    dmlist_foreach(g_services->services, libsystemd_start_service_visitor, NULL);

    return 0;
}

/**
 * @brief Parse a single ".ini" unit file into a newly allocated service, optionally merged with a template
 *
 * Shared implementation behind both the public libsystemd_parse_file() (which
 * always passes @p template_path/@p prefix/@p instance as NULL - a plain,
 * non-templated parse) and libsystemd_parse_dir()'s handling of
 * ::LIBSYSTEMD_UNIT_INSTANCE files.
 *
 * When @p template_path is non-NULL, it is parsed into the same `dmini`
 * context *first* - best-effort, a missing/invalid template file is not an
 * error, it just means the instance file's own keys are all there is - so
 * that @p file_path's own keys are layered on top and override the
 * template's for any key present in both (`dmini_parse_file`/
 * `dmini_set_string` update an existing key's value in place - see
 * `set_pair_in_section()` in dmini). When @p prefix/@p instance are both
 * non-NULL, every key's value is then run through
 * libsystemd_apply_specifiers() to expand `%i`/`%I`/`%p`/`%n`/`%%`.
 *
 * Otherwise behaves exactly like the original single-file parse: copies
 * "exec" (required), "args" (optional, tokenized into argv),
 * "stdin"/"stdout"/"stderr"/"stdlog" (optional, stream redirections), and
 * "requires"/"after" (optional, dependency name lists) into a fresh
 * `libsystemd_service_t`. Does **not** set `unit_name` or `starting_order` -
 * those are the caller's responsibility (see libsystemd_parse_dir()). Does
 * not touch the global registry.
 *
 * @param file_path     Path to the ".ini" file to parse (must not be NULL).
 * @param template_path Path to the file's `<prefix>@.ini` template, or NULL
 *                        for a non-templated parse.
 * @param prefix        Template prefix (e.g. "getty"), or NULL.
 * @param instance      Instance name (e.g. "tty1"), or NULL.
 * @param service       Receives the newly allocated service on success (must not be NULL).
 *
 * @retval 0       `*service` now points to a fully populated service (unit_name is NULL,
 *                   starting_order is 0, pid is -1).
 * @retval -EINVAL @p file_path or @p service was NULL, or the merged result has no "exec" key.
 * @retval -ENOMEM Allocation failed at some point during parsing; nothing is
 *                   leaked, and `*service` is left untouched.
 * @retval <0      Any other negative value is a `DMINI_ERR_*` code forwarded
 *                   from `dmini_parse_file` on @p file_path itself (e.g.
 *                   `DMINI_ERR_FILE` if the file could not be opened) - a
 *                   failure to parse @p template_path is never propagated.
 */
static int libsystemd_parse_unit_internal(const char* file_path, const char* template_path, const char* prefix, const char* instance, libsystemd_service_t* service)
{
    if (file_path == NULL || service == NULL)
    {
        return -EINVAL;
    }

    dmini_context_t ctx = dmini_create();
    if (ctx == NULL)
    {
        return -ENOMEM;
    }

    if (template_path != NULL)
    {
        dmini_parse_file(ctx, template_path);
    }

    int result = dmini_parse_file(ctx, file_path);
    if (result != DMINI_OK)
    {
        dmini_destroy(ctx);
        return result;
    }

    if (prefix != NULL && instance != NULL)
    {
        char* unit_name = libsystemd_build_unit_name(prefix, instance);
        if (unit_name == NULL || !libsystemd_apply_specifiers(ctx, prefix, instance, unit_name))
        {
            Dmod_Free(unit_name);
            dmini_destroy(ctx);
            return -ENOMEM;
        }
        Dmod_Free(unit_name);
    }

    result = libsystemd_build_service_from_ctx(ctx, service);
    dmini_destroy(ctx);

    return result;
}

/**
 * @brief Parse a single ".ini" unit file into a newly allocated service
 *
 * Thin wrapper around libsystemd_parse_unit_internal() with no template - see
 * that function for the full behavior. Kept as the public entry point so
 * templated instance parsing (only reachable from libsystemd_parse_dir(),
 * which knows a file's `<prefix>@.ini` template path and instance name)
 * stays an internal implementation detail.
 *
 * @param file_path Path to the ".ini" file to parse.
 * @param service   Receives the newly allocated service on success (must not be NULL).
 *
 * @retval 0       `*service` now points to a fully populated service (unit_name is NULL,
 *                   starting_order is 0, pid is -1).
 * @retval -EINVAL @p file_path or @p service was NULL, or the file has no "exec" key.
 * @retval -ENOMEM Allocation failed at some point during parsing; nothing is
 *                   leaked, and `*service` is left untouched.
 * @retval <0      Any other negative value is a `DMINI_ERR_*` code forwarded
 *                   from `dmini_parse_file` (e.g. `DMINI_ERR_FILE` if the file
 *                   could not be opened).
 *
 * @par Example
 * @code
 * libsystemd_service_t service = NULL;
 * int result = libsystemd_parse_file("/etc/services/webserver.ini", &service);
 * if (result == 0) {
 *     // service->exec, service->argc/argv, service->streams, ... are populated
 *     libsystemd_destroy_service(service); // if not handed off to a registry
 * }
 * @endcode
 */
dmod_libsystemd_api_declaration(1.0, int, _parse_file, ( const char* file_path, libsystemd_service_t* service ))
{
    return libsystemd_parse_unit_internal(file_path, NULL, NULL, NULL, service);
}

/**
 * @brief Parse every ".ini" unit file in a directory into a newly allocated registry
 *
 * Opens @p dir_path and, for each entry whose name ends in ".ini"
 * (libsystemd_has_ini_extension()), classifies it via
 * libsystemd_classify_unit_file():
 * - ::LIBSYSTEMD_UNIT_TEMPLATE (e.g. "getty@.ini") is skipped entirely - a
 *   bare template is never parsed or started on its own, exactly like
 *   `systemctl start foo@.service` is refused in real systemd.
 * - ::LIBSYSTEMD_UNIT_PLAIN and ::LIBSYSTEMD_UNIT_INSTANCE are parsed via
 *   libsystemd_parse_unit_internal() - for an instance (e.g. "getty@tty1.ini"),
 *   its `<prefix>@.ini` template (if any exists next to it) is merged in as
 *   defaults first and `%i`/`%I`/`%p`/`%n`/`%%` specifiers are expanded, so a
 *   near-empty instance file can inherit everything from the template and
 *   override only what differs (see [configuration.md](../docs/configuration.md#templates)).
 *
 * On success, sets the resulting service's `unit_name`
 * (libsystemd_make_unit_name() - "getty@tty1.ini" becomes "getty@tty1") and
 * appends it to the new registry's list. Files that fail to parse are
 * logged via DMOD_LOG_WARN() and skipped rather than aborting the whole scan.
 * Does not resolve `starting_order`, sort, or start anything - see
 * libsystemd_scan() for the full pipeline built on top of this function.
 *
 * @param dir_path Directory to scan (e.g. "/etc/services").
 * @param services Receives the newly allocated registry on success (must not be NULL).
 *
 * @retval 0       `*services` now holds every successfully parsed ".ini" file in
 *                   @p dir_path (possibly zero services if none were found/parsed).
 * @retval -EINVAL @p dir_path or @p services was NULL.
 * @retval -ENOENT @p dir_path does not exist / cannot be opened.
 * @retval -ENOMEM Allocation of the registry or its underlying dmlist failed.
 *
 * @note This function is atomic with respect to its own allocations: on
 *       failure it always cleans up whatever it allocated itself and leaves
 *       `*services` untouched, so callers never need to clean up after a
 *       failed call.
 *
 * @par Example
 * @code
 * libsystemd_services_t services = NULL;
 * int result = libsystemd_parse_dir("/etc/services", &services);
 * if (result == 0) {
 *     // services now owns every parsed unit; hand it to g_services or destroy it
 * }
 * @endcode
 */
dmod_libsystemd_api_declaration(1.0, int, _parse_dir, ( const char* dir_path, libsystemd_services_t* services ))
{
    if (dir_path == NULL || services == NULL)
    {
        return -EINVAL;
    }

    void* dir = Dmod_OpenDir(dir_path);
    if (dir == NULL)
    {
        return -ENOENT;
    }

    libsystemd_services_t new_services = Dmod_Malloc(sizeof(struct libsystemd_services));
    if (new_services == NULL)
    {
        Dmod_CloseDir(dir);
        return -ENOMEM;
    }

    new_services->services = dmlist_create(DMOD_MODULE_NAME);
    if (new_services->services == NULL)
    {
        Dmod_Free(new_services);
        Dmod_CloseDir(dir);
        return -ENOMEM;
    }

    const Dmod_DirEntry_t* entry = Dmod_ReadDirEx(dir);
    while (entry != NULL)
    {
        if (libsystemd_has_ini_extension(entry->name))
        {
            char* prefix = NULL;
            char* instance = NULL;
            libsystemd_unit_kind_t kind = libsystemd_classify_unit_file(entry->name, &prefix, &instance);

            if (kind != LIBSYSTEMD_UNIT_TEMPLATE)
            {
                char* file_path = libsystemd_join_path(dir_path, entry->name);
                if (file_path != NULL)
                {
                    char* template_path = (kind == LIBSYSTEMD_UNIT_INSTANCE) ? libsystemd_build_template_path(dir_path, prefix) : NULL;

                    libsystemd_service_t service = NULL;
                    int result = libsystemd_parse_unit_internal(file_path, template_path, prefix, instance, &service);
                    if (result == 0)
                    {
                        service->unit_name = libsystemd_make_unit_name(entry->name);
                        if (service->unit_name == NULL || !dmlist_push_back(new_services->services, service))
                        {
                            libsystemd_destroy_service(service);
                        }
                    }
                    else
                    {
                        DMOD_LOG_WARN("Failed to parse service file '%s' (%d)\n", file_path, result);
                    }

                    Dmod_Free(template_path);
                    Dmod_Free(file_path);
                }
            }

            Dmod_Free(prefix);
            Dmod_Free(instance);
        }

        entry = Dmod_ReadDirEx(dir);
    }

    Dmod_CloseDir(dir);

    *services = new_services;

    return 0;
}
