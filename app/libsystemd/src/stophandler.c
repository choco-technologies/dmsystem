#include "stophandler.h"
#include <errno.h>
#include "dmosi.h"
#include "dmlist.h"

/**
 * @brief Magic value of a live registry entry ('STOP')
 */
#define LIBSYSTEMD_STOPHANDLER_MAGIC 0x53544F50u

/**
 * @brief One process's stop handler registration
 *
 * Created by libsystemd_set_stop_handler() for the calling process and
 * removed when that process unregisters (NULL handler), when it terminates
 * (process exit callback, see libsystemd_stophandler_process_exited()), or
 * when the module is unloaded.
 */
typedef struct
{
    uint32_t magic;                                     //!< ::LIBSYSTEMD_STOPHANDLER_MAGIC while registered.
    dmosi_process_id_t pid;                             //!< Process that registered the handler.
    libsystemd_stop_handler_t handler;                  //!< Handler to call on a stop request.
    void* arg;                                          //!< Argument passed to @ref handler.
    dmosi_process_exit_callback_handle_t exit_handle;   //!< Exit callback that drops this entry when the process terminates.
} libsystemd_stophandler_t;

/**
 * @brief Registered stop handlers (list data pointers are libsystemd_stophandler_t)
 *
 * Unlike the unit registry in serviceapi.c, this list *is* touched from
 * several threads at once - services register from their own process while
 * the stopper (systemd, `service`, a device removal) looks handlers up - so
 * every access goes through @ref g_stophandlers_lock.
 */
static dmlist_context_t* g_stophandlers = NULL;

/**
 * @brief Guards @ref g_stophandlers and every entry in it
 */
static dmosi_mutex_t g_stophandlers_lock = NULL;

/**
 * @brief dmlist compare function matching an entry by process ID
 *
 * @param data1 Entry from the list (`libsystemd_stophandler_t*`).
 * @param data2 `const dmosi_process_id_t*` to look for.
 *
 * @retval 0  The entry belongs to that process.
 * @retval !0 It does not.
 */
static int libsystemd_stophandler_compare_pid(const void* data1, const void* data2)
{
    const libsystemd_stophandler_t* entry = (const libsystemd_stophandler_t*)data1;
    const dmosi_process_id_t* pid = (const dmosi_process_id_t*)data2;

    return (entry->pid == *pid) ? 0 : 1;
}

/**
 * @brief Lock held: remove and return the entry of @p pid, or NULL
 */
static libsystemd_stophandler_t* libsystemd_stophandler_take(dmosi_process_id_t pid)
{
    libsystemd_stophandler_t* entry = dmlist_find(g_stophandlers, &pid, libsystemd_stophandler_compare_pid);
    if (entry != NULL)
    {
        dmlist_remove(g_stophandlers, &pid, libsystemd_stophandler_compare_pid);
    }

    return entry;
}

/**
 * @brief Free an entry that is no longer in the list
 */
static void libsystemd_stophandler_free(libsystemd_stophandler_t* entry)
{
    entry->magic = 0;
    Dmod_Free(entry);
}

/**
 * @brief dmosi process exit callback: forget the handler of a terminated process
 *
 * The exit callback list of @p process is already detached by dmosi when this
 * runs, so the entry's @ref libsystemd_stophandler_t::exit_handle must not be
 * unregistered here - only the entry itself is dropped.
 *
 * @param process     Terminated process (unused - the entry carries its PID).
 * @param exit_status Unused.
 * @param arg         The `libsystemd_stophandler_t` registered for it.
 */
static void libsystemd_stophandler_process_exited(dmosi_process_t process, int exit_status, void* arg)
{
    (void)process;
    (void)exit_status;

    libsystemd_stophandler_t* registered = (libsystemd_stophandler_t*)arg;
    if (g_stophandlers_lock == NULL || registered == NULL || registered->magic != LIBSYSTEMD_STOPHANDLER_MAGIC)
    {
        return;
    }

    dmosi_mutex_lock(g_stophandlers_lock);
    libsystemd_stophandler_t* entry = libsystemd_stophandler_take(registered->pid);
    dmosi_mutex_unlock(g_stophandlers_lock);

    if (entry != NULL)
    {
        libsystemd_stophandler_free(entry);
    }
}

/**
 * @brief Lock held: register a new entry for @p process
 *
 * @retval 0       Registered.
 * @retval -ENOMEM Allocation or exit callback registration failed; nothing is left behind.
 */
static int libsystemd_stophandler_add(dmosi_process_t process, libsystemd_stop_handler_t handler, void* arg)
{
    libsystemd_stophandler_t* entry = Dmod_Malloc(sizeof(libsystemd_stophandler_t));
    if (entry == NULL)
    {
        return -ENOMEM;
    }

    entry->magic = LIBSYSTEMD_STOPHANDLER_MAGIC;
    entry->pid = dmosi_process_get_id(process);
    entry->handler = handler;
    entry->arg = arg;
    entry->exit_handle = dmosi_process_register_exit_callback(process, libsystemd_stophandler_process_exited, entry);

    if (entry->exit_handle == NULL || !dmlist_push_back(g_stophandlers, entry))
    {
        if (entry->exit_handle != NULL)
        {
            dmosi_process_unregister_exit_callback(process, entry->exit_handle);
        }
        libsystemd_stophandler_free(entry);
        return -ENOMEM;
    }

    return 0;
}

/**
 * @brief Lock held: drop the entry of @p process, if it has one
 */
static void libsystemd_stophandler_remove(dmosi_process_t process)
{
    libsystemd_stophandler_t* entry = libsystemd_stophandler_take(dmosi_process_get_id(process));
    if (entry == NULL)
    {
        return;
    }

    dmosi_process_unregister_exit_callback(process, entry->exit_handle);
    libsystemd_stophandler_free(entry);
}

int libsystemd_stophandler_init(void)
{
    if (g_stophandlers != NULL)
    {
        return 0;
    }

    g_stophandlers = dmlist_create();
    g_stophandlers_lock = dmosi_mutex_create(false);
    if (g_stophandlers == NULL || g_stophandlers_lock == NULL)
    {
        libsystemd_stophandler_deinit();
        return -ENOMEM;
    }

    return 0;
}

void libsystemd_stophandler_deinit(void)
{
    if (g_stophandlers != NULL)
    {
        libsystemd_stophandler_t* entry = dmlist_pop_front(g_stophandlers);
        while (entry != NULL)
        {
            dmosi_process_t process = dmosi_process_find_by_id(entry->pid);
            if (process != NULL)
            {
                dmosi_process_unregister_exit_callback(process, entry->exit_handle);
            }
            libsystemd_stophandler_free(entry);
            entry = dmlist_pop_front(g_stophandlers);
        }
        dmlist_destroy(g_stophandlers);
        g_stophandlers = NULL;
    }

    if (g_stophandlers_lock != NULL)
    {
        dmosi_mutex_destroy(g_stophandlers_lock);
        g_stophandlers_lock = NULL;
    }
}

bool libsystemd_stophandler_request(dmosi_process_id_t pid)
{
    if (g_stophandlers_lock == NULL)
    {
        return false;
    }

    dmosi_mutex_lock(g_stophandlers_lock);
    libsystemd_stophandler_t* entry = dmlist_find(g_stophandlers, &pid, libsystemd_stophandler_compare_pid);
    bool requested = (entry != NULL);
    if (requested)
    {
        entry->handler(entry->arg);
    }
    dmosi_mutex_unlock(g_stophandlers_lock);

    return requested;
}

/**
 * @brief Register (or clear) the calling process's stop request handler
 *
 * By default libsystemd_stop_service() ends a unit with dmosi_process_kill():
 * the process's threads are deleted wherever they happen to be, so a lock the
 * service holds at that moment (a driver's I/O lock, a filesystem's
 * metadata) stays held forever. A service that registers a handler is asked
 * first instead: libsystemd calls @p handler, then waits up to the unit's
 * `stop_timeout_ms` for the process to exit on its own (return from main()),
 * and only kills it if it has not.
 *
 * The registration belongs to the calling process - whatever unit tracks it
 * as its main PID (including one adopted via libsystemd_notify_main_pid()).
 * It is dropped automatically when the process terminates.
 *
 * @param handler Called on a stop request, in the stopper's context. It must
 *                only make the service leave (set a flag, post a semaphore
 *                the service waits on) and return at once - no blocking, no
 *                calls into libsystemd. NULL clears the registration.
 * @param arg     Passed to @p handler. It must stay valid until the handler
 *                is cleared or the process has terminated: clear it (NULL
 *                handler) before freeing @p arg - that call waits for a
 *                handler invocation in progress to return.
 *
 * @retval 0       Registered, replaced, or cleared (clearing an absent registration is not an error).
 * @retval -ESRCH  The caller is not a dmosi process.
 * @retval -ENOMEM Allocation failed, or the libsystemd module is not initialized.
 *
 * @par Example
 * @code
 * static void on_stop(void* arg)
 * {
 *     service_t* s = arg;
 *     s->stop = true;
 *     dmosi_semaphore_post(s->wakeup, 1);
 * }
 *
 * libsystemd_set_stop_handler(on_stop, &s);
 * while (!s.stop) {
 *     dmosi_semaphore_wait(s.wakeup, 1, -1);
 *     ...
 * }
 * libsystemd_set_stop_handler(NULL, NULL);
 * cleanup(&s);
 * return 0;
 * @endcode
 */
dmod_libsystemd_api_declaration(1.0, int, _set_stop_handler, ( libsystemd_stop_handler_t handler, void* arg ))
{
    dmosi_process_t self = dmosi_process_current();
    if (self == NULL)
    {
        return -ESRCH;
    }
    if (g_stophandlers_lock == NULL)
    {
        return -ENOMEM;
    }

    dmosi_process_id_t pid = dmosi_process_get_id(self);
    int result = 0;

    dmosi_mutex_lock(g_stophandlers_lock);
    libsystemd_stophandler_t* entry = dmlist_find(g_stophandlers, &pid, libsystemd_stophandler_compare_pid);
    if (handler == NULL)
    {
        libsystemd_stophandler_remove(self);
    }
    else if (entry != NULL)
    {
        entry->handler = handler;
        entry->arg = arg;
    }
    else
    {
        result = libsystemd_stophandler_add(self, handler, arg);
    }
    dmosi_mutex_unlock(g_stophandlers_lock);

    return result;
}
