#include "stoprequest.h"
#include <errno.h>
#include "dmosi.h"
#include "dmlist.h"

/**
 * @brief Magic value of a live registry entry ('STOP')
 */
#define LIBSYSTEMD_STOPREQUEST_MAGIC 0x53544F50u

/**
 * @brief One process's registration for stop requests
 *
 * Created by libsystemd_set_stop_semaphore() for the calling process and
 * removed when that process unregisters (NULL semaphore), when it terminates
 * (process exit callback, see libsystemd_stoprequest_process_exited()), or
 * when the module is unloaded.
 */
typedef struct
{
    uint32_t magic;                                     //!< ::LIBSYSTEMD_STOPREQUEST_MAGIC while registered.
    dmosi_process_id_t pid;                             //!< Process that registered.
    dmosi_semaphore_t wakeup;                           //!< Posted once per stop request.
    bool stop_requested;                                //!< Set by a stop request, read by libsystemd_stop_requested().
    dmosi_process_exit_callback_handle_t exit_handle;   //!< Exit callback that drops this entry when the process terminates.
} libsystemd_stoprequest_t;

/**
 * @brief Registered processes (list data pointers are libsystemd_stoprequest_t)
 *
 * Unlike the unit registry in serviceapi.c, this list *is* touched from
 * several threads at once - services register and poll from their own
 * process while the stopper (systemd, `service`, a device removal) sends
 * requests - so every access goes through @ref g_stoprequests_lock.
 *
 * The lock is only ever held around list operations, the flag and a
 * semaphore post, none of which can block, and never around code of a
 * service - so a misbehaving service cannot hold up a stop.
 */
static dmlist_context_t* g_stoprequests = NULL;

/**
 * @brief Guards @ref g_stoprequests and every entry in it
 */
static dmosi_mutex_t g_stoprequests_lock = NULL;

/**
 * @brief dmlist compare function matching an entry by process ID
 *
 * @param data1 Entry from the list (`libsystemd_stoprequest_t*`).
 * @param data2 `const dmosi_process_id_t*` to look for.
 *
 * @retval 0  The entry belongs to that process.
 * @retval !0 It does not.
 */
static int libsystemd_stoprequest_compare_pid(const void* data1, const void* data2)
{
    const libsystemd_stoprequest_t* entry = (const libsystemd_stoprequest_t*)data1;
    const dmosi_process_id_t* pid = (const dmosi_process_id_t*)data2;

    return (entry->pid == *pid) ? 0 : 1;
}

/**
 * @brief Lock held: find the entry of @p pid, or NULL
 */
static libsystemd_stoprequest_t* libsystemd_stoprequest_find(dmosi_process_id_t pid)
{
    return dmlist_find(g_stoprequests, &pid, libsystemd_stoprequest_compare_pid);
}

/**
 * @brief Lock held: remove and return the entry of @p pid, or NULL
 */
static libsystemd_stoprequest_t* libsystemd_stoprequest_take(dmosi_process_id_t pid)
{
    libsystemd_stoprequest_t* entry = libsystemd_stoprequest_find(pid);
    if (entry != NULL)
    {
        dmlist_remove(g_stoprequests, &pid, libsystemd_stoprequest_compare_pid);
    }

    return entry;
}

/**
 * @brief Free an entry that is no longer in the list
 */
static void libsystemd_stoprequest_free(libsystemd_stoprequest_t* entry)
{
    entry->magic = 0;
    Dmod_Free(entry);
}

/**
 * @brief The calling process, with its ID, or NULL if the caller is not a dmosi process
 */
static dmosi_process_t libsystemd_stoprequest_self(dmosi_process_id_t* pid)
{
    dmosi_process_t self = dmosi_process_current();
    if (self != NULL)
    {
        *pid = dmosi_process_get_id(self);
    }

    return self;
}

/**
 * @brief dmosi process exit callback: forget a terminated process
 *
 * The exit callback list of @p process is already detached by dmosi when this
 * runs, so the entry's exit handle must not be unregistered here - only the
 * entry itself is dropped.
 *
 * @param process     Terminated process (unused - the entry carries its PID).
 * @param exit_status Unused.
 * @param arg         The `libsystemd_stoprequest_t` registered for it.
 */
static void libsystemd_stoprequest_process_exited(dmosi_process_t process, int exit_status, void* arg)
{
    (void)process;
    (void)exit_status;

    libsystemd_stoprequest_t* registered = (libsystemd_stoprequest_t*)arg;
    if (g_stoprequests_lock == NULL || registered == NULL || registered->magic != LIBSYSTEMD_STOPREQUEST_MAGIC)
    {
        return;
    }

    dmosi_mutex_lock(g_stoprequests_lock);
    libsystemd_stoprequest_t* entry = libsystemd_stoprequest_take(registered->pid);
    dmosi_mutex_unlock(g_stoprequests_lock);

    if (entry != NULL)
    {
        libsystemd_stoprequest_free(entry);
    }
}

/**
 * @brief Lock held: register a new entry for @p process
 *
 * @retval 0       Registered.
 * @retval -ENOMEM Allocation or exit callback registration failed; nothing is left behind.
 */
static int libsystemd_stoprequest_add(dmosi_process_t process, dmosi_semaphore_t wakeup)
{
    libsystemd_stoprequest_t* entry = Dmod_Malloc(sizeof(libsystemd_stoprequest_t));
    if (entry == NULL)
    {
        return -ENOMEM;
    }

    entry->magic = LIBSYSTEMD_STOPREQUEST_MAGIC;
    entry->pid = dmosi_process_get_id(process);
    entry->wakeup = wakeup;
    entry->stop_requested = false;
    entry->exit_handle = dmosi_process_register_exit_callback(process, libsystemd_stoprequest_process_exited, entry);

    if (entry->exit_handle == NULL || !dmlist_push_back(g_stoprequests, entry))
    {
        if (entry->exit_handle != NULL)
        {
            dmosi_process_unregister_exit_callback(process, entry->exit_handle);
        }
        libsystemd_stoprequest_free(entry);
        return -ENOMEM;
    }

    return 0;
}

/**
 * @brief Lock held: drop the entry of @p process, if it has one
 */
static void libsystemd_stoprequest_remove(dmosi_process_t process, dmosi_process_id_t pid)
{
    libsystemd_stoprequest_t* entry = libsystemd_stoprequest_take(pid);
    if (entry == NULL)
    {
        return;
    }

    dmosi_process_unregister_exit_callback(process, entry->exit_handle);
    libsystemd_stoprequest_free(entry);
}

int libsystemd_stoprequest_init(void)
{
    if (g_stoprequests != NULL)
    {
        return 0;
    }

    g_stoprequests = dmlist_create();
    g_stoprequests_lock = dmosi_mutex_create(false);
    if (g_stoprequests == NULL || g_stoprequests_lock == NULL)
    {
        libsystemd_stoprequest_deinit();
        return -ENOMEM;
    }

    return 0;
}

void libsystemd_stoprequest_deinit(void)
{
    if (g_stoprequests != NULL)
    {
        libsystemd_stoprequest_t* entry = dmlist_pop_front(g_stoprequests);
        while (entry != NULL)
        {
            dmosi_process_t process = dmosi_process_find_by_id(entry->pid);
            if (process != NULL)
            {
                dmosi_process_unregister_exit_callback(process, entry->exit_handle);
            }
            libsystemd_stoprequest_free(entry);
            entry = dmlist_pop_front(g_stoprequests);
        }
        dmlist_destroy(g_stoprequests);
        g_stoprequests = NULL;
    }

    if (g_stoprequests_lock != NULL)
    {
        dmosi_mutex_destroy(g_stoprequests_lock);
        g_stoprequests_lock = NULL;
    }
}

bool libsystemd_stoprequest_send(dmosi_process_id_t pid)
{
    if (g_stoprequests_lock == NULL)
    {
        return false;
    }

    dmosi_mutex_lock(g_stoprequests_lock);
    libsystemd_stoprequest_t* entry = libsystemd_stoprequest_find(pid);
    if (entry != NULL)
    {
        entry->stop_requested = true;
        dmosi_semaphore_post(entry->wakeup, 1);  /* never blocks; a full semaphore already wakes it */
    }
    dmosi_mutex_unlock(g_stoprequests_lock);

    return entry != NULL;
}

/**
 * @brief Register (or clear) the semaphore that wakes the calling process on a stop request
 *
 * By default libsystemd_stop_service() ends a unit with dmosi_process_kill():
 * the process's threads are deleted wherever they happen to be, so a lock the
 * service holds at that moment (a driver's I/O lock, a filesystem's
 * metadata) stays held forever. A process that registers here is asked
 * first instead: libsystemd sets its "stop requested" flag
 * (libsystemd_stop_requested()), posts @p wakeup once, then waits up to the
 * unit's `stop_timeout_ms` for the process to exit on its own (return from
 * main()), and only kills it if it has not.
 *
 * No code of the service ever runs in the stopper's context, so a service
 * that hangs or ignores the request cannot hold up the stop - it is killed
 * once the timeout expires.
 *
 * The registration belongs to the calling process - whatever unit tracks it
 * as its main PID (including one adopted via libsystemd_notify_main_pid()).
 * It is dropped automatically when the process terminates. Registering again
 * replaces the semaphore and keeps a stop request already received.
 *
 * @param wakeup Semaphore the service waits on in its main loop (it may be the
 *               one it already waits on for its own events). NULL clears the
 *               registration - do that before destroying the semaphore; the
 *               call waits for a post in progress to finish.
 *
 * @retval 0       Registered, replaced, or cleared (clearing an absent registration is not an error).
 * @retval -ESRCH  The caller is not a dmosi process.
 * @retval -ENOMEM Allocation failed, or the libsystemd module is not initialized.
 *
 * @par Example
 * @code
 * libsystemd_set_stop_semaphore(wakeup);
 * while (!libsystemd_stop_requested()) {
 *     dmosi_semaphore_wait(wakeup, 1, -1);
 *     ...
 * }
 * libsystemd_set_stop_semaphore(NULL);
 * dmosi_semaphore_destroy(wakeup);
 * return 0;
 * @endcode
 */
dmod_libsystemd_api_declaration(1.0, int, _set_stop_semaphore, ( dmosi_semaphore_t wakeup ))
{
    dmosi_process_id_t pid = 0;
    dmosi_process_t self = libsystemd_stoprequest_self(&pid);
    if (self == NULL)
    {
        return -ESRCH;
    }
    if (g_stoprequests_lock == NULL)
    {
        return -ENOMEM;
    }

    int result = 0;
    dmosi_mutex_lock(g_stoprequests_lock);
    libsystemd_stoprequest_t* entry = libsystemd_stoprequest_find(pid);
    if (wakeup == NULL)
    {
        libsystemd_stoprequest_remove(self, pid);
    }
    else if (entry != NULL)
    {
        entry->wakeup = wakeup;
    }
    else
    {
        result = libsystemd_stoprequest_add(self, wakeup);
    }
    dmosi_mutex_unlock(g_stoprequests_lock);

    return result;
}

/**
 * @brief Whether a stop was requested for the calling process
 *
 * Check it in the service's main loop, after waking up on the semaphore
 * registered with libsystemd_set_stop_semaphore().
 *
 * @retval true  libsystemd_stop_service() asked the calling process to stop.
 * @retval false No stop was requested, or the caller never registered.
 */
dmod_libsystemd_api_declaration(1.0, bool, _stop_requested, ( void ))
{
    dmosi_process_id_t pid = 0;
    if (libsystemd_stoprequest_self(&pid) == NULL || g_stoprequests_lock == NULL)
    {
        return false;
    }

    dmosi_mutex_lock(g_stoprequests_lock);
    libsystemd_stoprequest_t* entry = libsystemd_stoprequest_find(pid);
    bool requested = (entry != NULL) && entry->stop_requested;
    dmosi_mutex_unlock(g_stoprequests_lock);

    return requested;
}
