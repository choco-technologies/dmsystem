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
 * An instance is created by libsystemd_parse_file() (called from libsystemd_parse_dir())
 * and lives inside the global service registry (@ref g_services) until it is
 * replaced by a new libsystemd_scan() or the module is unloaded (libsystemd_serviceapi_deinit()).
 */
struct libsystemd_service
{
    char* unit_name;                    //!< Unit name, derived from the ini file name (without the ".ini" suffix). Owned copy.
    char* exec;                         //!< Module name (or file path) to spawn, from the "exec" key. Owned copy.
    int argc;                           //!< Number of entries in argv (always >= 1, argv[0] == exec).
    char** argv;                        //!< NULL-terminated argument vector (argc+1 entries, each an owned copy).
    Dmod_StreamRedirections_t streams;  //!< Stream redirections built from the optional "stdin"/"stdout"/"stderr" keys.
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
 * @brief Build a service's stream redirection table from its "stdin"/"stdout"/"stderr" ini keys
 *
 * Allocates up to 3 `Dmod_StreamRedirection_t` entries, one for each of
 * "stdin", "stdout", "stderr" that is present in @p ctx, mapping it to
 * `DMOD_STDIN`/`DMOD_STDOUT`/`DMOD_STDERR` respectively with an owned copy of
 * its path. Keys that are absent are simply skipped - `service->streams.Count`
 * reflects only the keys that were actually present.
 *
 * @param ctx     Parsed ini context to read the keys from (must not be NULL).
 * @param service Service being built; `streams` is set on success (must not be NULL).
 *
 * @retval true  `service->streams` was populated successfully (possibly with `Count == 0`
 *                and `Entries == NULL` if none of the three keys were present).
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

    Dmod_StreamRedirection_t* entries = Dmod_Malloc(sizeof(Dmod_StreamRedirection_t) * 3);
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
 * @brief Start a previously scanned service by unit name
 *
 * Looks @p unit_name up in the global registry (@ref g_services, populated by
 * libsystemd_scan()) and, if found, spawns it via libsystemd_start_service_internal().
 *
 * @param unit_name Unit name to start (e.g. "webserver"), as derived by
 *                    libsystemd_parse_dir() from the ini file name.
 *
 * @retval 0         The service was found and spawned successfully.
 * @retval -EINVAL   @p unit_name was NULL.
 * @retval -ENOENT   No service with that unit name exists in the registry
 *                     (including the case where libsystemd_scan() was never called).
 * @retval -EALREADY The service already has a live process associated with it.
 * @retval -ENOSYS   Module spawning is not available on this build/platform.
 * @retval <0        Any other negative value forwarded from `Dmod_SpawnModule`.
 *
 * @par Example
 * @code
 * libsystemd_scan("/etc/services");
 * int result = libsystemd_start_service("webserver");
 * if (result != 0) {
 *     Dmod_Printf("failed to start webserver: %d\n", result);
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
 * 6. Installs the fresh, sorted registry as the new @ref g_services.
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

    dmlist_foreach(g_services->services, libsystemd_start_service_visitor, NULL);

    return 0;
}

/**
 * @brief Parse a single ".ini" unit file into a newly allocated service
 *
 * Reads @p file_path via `dmini` and copies every field this module
 * understands into a fresh `libsystemd_service_t`: "exec" (required), "args"
 * (optional, tokenized into argv), "stdin"/"stdout"/"stderr" (optional,
 * stream redirections), and "requires"/"after" (optional, dependency name
 * lists). Does **not** set `unit_name` or `starting_order` - those are the
 * responsibility of the caller (see libsystemd_parse_dir()), since a bare ini
 * file has no notion of its own file name or its place relative to other
 * services. Does not touch the global registry.
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
    if (file_path == NULL || service == NULL)
    {
        return -EINVAL;
    }

    dmini_context_t ctx = dmini_create();
    if (ctx == NULL)
    {
        return -ENOMEM;
    }

    int result = dmini_parse_file(ctx, file_path);
    if (result != DMINI_OK)
    {
        dmini_destroy(ctx);
        return result;
    }

    const char* exec = dmini_get_string(ctx, NULL, "exec", NULL);
    if (exec == NULL)
    {
        dmini_destroy(ctx);
        return -EINVAL;
    }

    libsystemd_service_t new_service = Dmod_Malloc(sizeof(struct libsystemd_service));
    if (new_service == NULL)
    {
        dmini_destroy(ctx);
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

    dmini_destroy(ctx);

    if (!ok)
    {
        libsystemd_destroy_service(new_service);
        return -ENOMEM;
    }

    *service = new_service;

    return 0;
}

/**
 * @brief Parse every ".ini" unit file in a directory into a newly allocated registry
 *
 * Opens @p dir_path and, for each entry whose name ends in ".ini"
 * (libsystemd_has_ini_extension()), calls libsystemd_parse_file() on it and, on
 * success, sets the resulting service's `unit_name` (libsystemd_make_unit_name())
 * and appends it to the new registry's list. Files that fail to parse are
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
            char* file_path = libsystemd_join_path(dir_path, entry->name);
            if (file_path != NULL)
            {
                libsystemd_service_t service = NULL;
                int result = libsystemd_parse_file(file_path, &service);
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
                Dmod_Free(file_path);
            }
        }

        entry = Dmod_ReadDirEx(dir);
    }

    Dmod_CloseDir(dir);

    *services = new_services;

    return 0;
}
