/**
 * @file libsystemd.c
 * @brief The front of libsystemd: the whole API, a queue for what comes before the implementation is there
 *
 * libsystemd is small on purpose: it is loaded together with the first module that reports a device (dmdevfs, dmtty), which is
 * early in the boot, when only the internal RAM of a small chip can hold it. The units, the rules and the supervision of the
 * processes are in `libsystemd_core`, which is loaded with the first service manager (systemd, service) - from where the
 * allocations go to wherever there is room.
 *
 * Until then the reports of the devices and of the mounts are kept (the way the implementation keeps the devices no rule matches
 * yet) and the rest of the calls answer -ENODEV. libsystemd_core implements the dif of libsystemd.h (_impl_*) and calls
 * libsystemd_deliver_reports() when it is ready: the kept reports are given to it, in the order they came, and from then on every
 * call is passed on through the dif.
 */
#define DMOD_ENABLE_REGISTRATION ON
#define ENABLE_DIF_REGISTRATIONS
#include "libsystemd.h"
#include "libsystemd_service.h"
#include <errno.h>
#include <string.h>
#include "dmosi.h"
#include "dmfsi.h"

/**
 * @brief What was reported before the implementation was attached
 */
typedef enum
{
    LIBSYSTEMD_REPORT_ADDED,        //!< libsystemd_notify_device_added(class, name, value)
    LIBSYSTEMD_REPORT_REMOVED,      //!< libsystemd_notify_device_removed(class, name)
    LIBSYSTEMD_REPORT_MOUNTED,      //!< the dif _mount_changed(name = mount point, mounted = true)
    LIBSYSTEMD_REPORT_UNMOUNTED,    //!< the dif _mount_changed(name = mount point, mounted = false)
} libsystemd_report_kind_t;

typedef struct libsystemd_report
{
    struct libsystemd_report*   next;
    libsystemd_report_kind_t    kind;
    char*                       device_class;   //!< NULL for a mount
    char*                       name;           //!< device name, or the mount point
    char*                       value;          //!< value of the device (%v), or NULL
} libsystemd_report_t;

/**
 * @brief The most reports kept: a driver that reports the same device again and again does not fill the memory
 */
#define LIBSYSTEMD_REPORTS_MAX  64u

static dmosi_mutex_t                g_lock          = NULL;     //!< guards the rest (held while the list or a flag changes, never during a call)
static bool                         g_ready         = false;    //!< the implementation took the reports: calls are passed on
static bool                         g_delivering    = false;    //!< the reports are being given to the implementation
static libsystemd_report_t*         g_head          = NULL;     //!< the reports kept, the oldest first
static libsystemd_report_t*         g_tail          = NULL;
static size_t                       g_count         = 0;

static void lock(void)   { if (g_lock != NULL) { dmosi_mutex_lock(g_lock); } }
static void unlock(void) { if (g_lock != NULL) { dmosi_mutex_unlock(g_lock); } }

static void report_destroy(libsystemd_report_t* report)
{
    if (report != NULL)
    {
        Dmod_Free(report->device_class);
        Dmod_Free(report->name);
        Dmod_Free(report->value);
        Dmod_Free(report);
    }
}

static libsystemd_report_t* report_create(libsystemd_report_kind_t kind, const char* device_class, const char* name, const char* value)
{
    libsystemd_report_t* report = Dmod_Malloc(sizeof(*report));
    if (report == NULL)
    {
        return NULL;
    }
    memset(report, 0, sizeof(*report));
    report->kind         = kind;
    report->device_class = (device_class != NULL) ? Dmod_StrDup(device_class) : NULL;
    report->name         = Dmod_StrDup(name);
    report->value        = (value != NULL) ? Dmod_StrDup(value) : NULL;
    if (report->name == NULL || (device_class != NULL && report->device_class == NULL) || (value != NULL && report->value == NULL))
    {
        report_destroy(report);
        return NULL;
    }
    return report;
}

/** @brief Take the first report off the list (the lock held), NULL when there is none */
static libsystemd_report_t* report_pop(void)
{
    libsystemd_report_t* report = g_head;
    if (report != NULL)
    {
        g_head = report->next;
        if (g_head == NULL)
        {
            g_tail = NULL;
        }
        g_count--;
        report->next = NULL;
    }
    return report;
}

static void report_push(libsystemd_report_t* report)
{
    if (g_tail != NULL)
    {
        g_tail->next = report;
    }
    else
    {
        g_head = report;
    }
    g_tail = report;
    g_count++;
}

/** @brief Find the report of the device in the list (the lock held): the same kind, class and name */
static libsystemd_report_t** report_find(libsystemd_report_kind_t kind, const char* device_class, const char* name)
{
    for (libsystemd_report_t** link = &g_head; *link != NULL; link = &(*link)->next)
    {
        libsystemd_report_t* report = *link;
        if (report->kind == kind && strcmp(report->name, name) == 0 && strcmp(report->device_class, device_class) == 0)
        {
            return link;
        }
    }
    return NULL;
}

static void report_unlink(libsystemd_report_t** link)
{
    libsystemd_report_t* report = *link;
    *link = report->next;
    if (g_tail == report)
    {
        g_tail = NULL;
        for (libsystemd_report_t* last = g_head; last != NULL; last = last->next)
        {
            g_tail = last;
        }
    }
    g_count--;
    report_destroy(report);
}

/**
 * @brief The function of the dif the implementation offers, NULL when the calls are not passed on (yet, or any more)
 *
 * The implementation is found when the call comes: a module that is not loaded cannot be called, and the reports are kept again.
 */
static void* impl_function(const char* signature)
{
    Dmod_Context_t* impl = Dmod_GetNextDifModule(signature, NULL);
    return (impl != NULL) ? Dmod_GetDifFunction(impl, signature) : NULL;
}

static int deliver_reports(void);

static void* ready_function(const char* signature)
{
    lock();
    bool ready = g_ready;
    unlock();
    if (!ready && deliver_reports() != 0)
    {
        // No implementation yet (or it is being given the reports): the caller keeps the call or answers -ENODEV
        return NULL;
    }
    void* function = impl_function(signature);
    if (function == NULL)
    {
        lock();
        g_ready = false;
        unlock();
    }
    return function;
}

/**
 * @brief Keep a report of a device, unless the implementation is there by now
 *
 * A device removed while the report of its adding is still kept cancels it: with no rules there was nothing to do for it,
 * and the implementation forgets such a device the same way.
 *
 * @return 1 the implementation is there: pass the call on, 0 kept (or dropped), -ENOMEM
 */
static int device_report_keep(libsystemd_report_kind_t kind, const char* device_class, const char* name, const char* value)
{
    lock();
    if (g_ready)
    {
        unlock();
        return 1;
    }

    int result = 0;
    if (kind == LIBSYSTEMD_REPORT_REMOVED)
    {
        libsystemd_report_t** added = report_find(LIBSYSTEMD_REPORT_ADDED, device_class, name);
        if (added != NULL)
        {
            report_unlink(added);
            unlock();
            return 0;
        }
    }
    else if (report_find(LIBSYSTEMD_REPORT_ADDED, device_class, name) != NULL)
    {
        unlock();
        return 0;
    }

    if (g_count >= LIBSYSTEMD_REPORTS_MAX)
    {
        DMOD_LOG_WARN("libsystemd: %u reports are kept and nothing takes them - '%s/%s' is lost\n", (unsigned)g_count, device_class, name);
        unlock();
        return -ENOMEM;
    }

    libsystemd_report_t* report = report_create(kind, device_class, name, value);
    if (report == NULL)
    {
        result = -ENOMEM;
    }
    else
    {
        report_push(report);
    }
    unlock();
    return result;
}

/** @brief Pass a call on to the implementation, or answer NO_IMPL when it is not there */
#define LIBSYSTEMD_PASS_ON(NAME, NO_IMPL, ...)                                              \
    dmod_libsystemd_impl_##NAME##_t function = (dmod_libsystemd_impl_##NAME##_t)ready_function(dmod_libsystemd_impl_##NAME##_sig); \
    return (function != NULL) ? function(__VA_ARGS__) : (NO_IMPL)

dmod_libsystemd_api_declaration(1.0, int, _notify_device_added, (const char* device_class, const char* device_name, const char* user_value))
{
    if (device_class == NULL || device_name == NULL)
    {
        return -EINVAL;
    }
    for (;;)
    {
        int kept = device_report_keep(LIBSYSTEMD_REPORT_ADDED, device_class, device_name, user_value);
        if (kept != 1)
        {
            // Kept for the implementation: the same answer as "no rule is loaded yet"
            return (kept == 0) ? -ENOENT : kept;
        }
        dmod_libsystemd_impl_notify_device_added_t function = (dmod_libsystemd_impl_notify_device_added_t)ready_function(dmod_libsystemd_impl_notify_device_added_sig);
        if (function != NULL)
        {
            return function(device_class, device_name, user_value);
        }
        // The implementation was unloaded: keep the report (the loop asks again)
    }
}

dmod_libsystemd_api_declaration(1.0, int, _notify_device_removed, (const char* device_class, const char* device_name))
{
    if (device_class == NULL || device_name == NULL)
    {
        return -EINVAL;
    }
    for (;;)
    {
        int kept = device_report_keep(LIBSYSTEMD_REPORT_REMOVED, device_class, device_name, NULL);
        if (kept != 1)
        {
            return (kept == 0) ? -ENOENT : kept;
        }
        dmod_libsystemd_impl_notify_device_removed_t function = (dmod_libsystemd_impl_notify_device_removed_t)ready_function(dmod_libsystemd_impl_notify_device_removed_sig);
        if (function != NULL)
        {
            return function(device_class, device_name);
        }
    }
}

/**
 * @brief dmfsi's _mount_changed dif (called by dmvfs): the mount is kept until the implementation is there
 */
dmod_dmfsi_dif_api_declaration( 1.0, libsystemd, void, _mount_changed, (const char* mount_point, bool mounted) )
{
    if (mount_point == NULL)
    {
        return;
    }

    dmod_libsystemd_impl_mount_changed_t function = (dmod_libsystemd_impl_mount_changed_t)ready_function(dmod_libsystemd_impl_mount_changed_sig);
    if (function != NULL)
    {
        function(mount_point, mounted);
        return;
    }

    lock();
    libsystemd_report_t* report = NULL;
    if (g_count < LIBSYSTEMD_REPORTS_MAX)
    {
        report = report_create(mounted ? LIBSYSTEMD_REPORT_MOUNTED : LIBSYSTEMD_REPORT_UNMOUNTED, NULL, mount_point, NULL);
    }
    if (report != NULL)
    {
        report_push(report);
    }
    else
    {
        DMOD_LOG_WARN("libsystemd: the %s of '%s' is lost (no room for the report)\n", mounted ? "mount" : "unmount", mount_point);
    }
    unlock();
}

/** @brief The functions of the dif the implementation offers, found for the delivery of the reports */
typedef struct
{
    dmod_libsystemd_impl_notify_device_added_t   added;
    dmod_libsystemd_impl_notify_device_removed_t removed;
    dmod_libsystemd_impl_mount_changed_t         mount_changed;
} libsystemd_sink_t;

static void report_deliver(const libsystemd_sink_t* sink, const libsystemd_report_t* report)
{
    switch (report->kind)
    {
        case LIBSYSTEMD_REPORT_ADDED:      sink->added(report->device_class, report->name, report->value); break;
        case LIBSYSTEMD_REPORT_REMOVED:    sink->removed(report->device_class, report->name); break;
        case LIBSYSTEMD_REPORT_MOUNTED:    sink->mount_changed(report->name, true); break;
        case LIBSYSTEMD_REPORT_UNMOUNTED:  sink->mount_changed(report->name, false); break;
    }
}

static int deliver_reports(void)
{
    libsystemd_sink_t sink;
    sink.added         = (dmod_libsystemd_impl_notify_device_added_t)impl_function(dmod_libsystemd_impl_notify_device_added_sig);
    sink.removed       = (dmod_libsystemd_impl_notify_device_removed_t)impl_function(dmod_libsystemd_impl_notify_device_removed_sig);
    sink.mount_changed = (dmod_libsystemd_impl_mount_changed_t)impl_function(dmod_libsystemd_impl_mount_changed_sig);
    if (sink.added == NULL || sink.removed == NULL || sink.mount_changed == NULL)
    {
        return -ENODEV;
    }

    lock();
    if (g_delivering)
    {
        unlock();
        return -EALREADY;
    }
    g_delivering = true;

    // The reports are given in the order they came, with the lock released (the implementation may start a service that reports
    // again); what comes meanwhile is kept behind them, and the calls are passed on when nothing is left.
    libsystemd_report_t* report = report_pop();
    while (report != NULL)
    {
        unlock();
        report_deliver(&sink, report);
        report_destroy(report);
        lock();
        report = report_pop();
    }
    g_ready      = true;
    g_delivering = false;
    unlock();
    return 0;
}

dmod_libsystemd_api_declaration(1.0, int, _deliver_reports, (void))
{
    return deliver_reports();
}

/**
 * The rest of the API needs the units: with no implementation (libsystemd_core is not loaded) there is nothing to do, and
 * the caller is told so.
 */
dmod_libsystemd_api_declaration(1.0, int, _start_service, ( const char* unit_name, const char* user_value ))
{
    LIBSYSTEMD_PASS_ON(start_service, -ENODEV, unit_name, user_value);
}

dmod_libsystemd_api_declaration(1.0, int, _stop_service, ( const char* unit_name ))
{
    LIBSYSTEMD_PASS_ON(stop_service, -ENODEV, unit_name);
}

dmod_libsystemd_api_declaration(1.0, int, _notify_main_pid, ( const char* unit_name, Dmod_Pid_t pid ))
{
    LIBSYSTEMD_PASS_ON(notify_main_pid, -ENODEV, unit_name, pid);
}

dmod_libsystemd_api_declaration(1.0, int, _set_stop_semaphore, ( dmosi_semaphore_t wakeup ))
{
    LIBSYSTEMD_PASS_ON(set_stop_semaphore, -ENODEV, wakeup);
}

dmod_libsystemd_api_declaration(1.0, bool, _stop_requested, ( void ))
{
    LIBSYSTEMD_PASS_ON(stop_requested, false);
}

dmod_libsystemd_api_declaration(1.0, int, _status, ( const char* unit_name, libsystemd_service_status_t* out_status ))
{
    LIBSYSTEMD_PASS_ON(status, -ENODEV, unit_name, out_status);
}

dmod_libsystemd_api_declaration(1.0, int, _list, (libsystemd_visitor_t visitor, void* user_ptr))
{
    LIBSYSTEMD_PASS_ON(list, -ENODEV, visitor, user_ptr);
}

dmod_libsystemd_api_declaration(1.0, int, _scan, (const char* path))
{
    LIBSYSTEMD_PASS_ON(scan, -ENODEV, path);
}

dmod_libsystemd_api_declaration(1.0, int, _parse_file, ( const char* file_path, libsystemd_service_t* service ))
{
    LIBSYSTEMD_PASS_ON(parse_file, -ENODEV, file_path, service);
}

dmod_libsystemd_api_declaration(1.0, int, _parse_dir, ( const char* dir_path, libsystemd_services_t* services ))
{
    LIBSYSTEMD_PASS_ON(parse_dir, -ENODEV, dir_path, services);
}

dmod_libsystemd_api_declaration(1.0, int, _load_rules, (const char* rules_dir))
{
    LIBSYSTEMD_PASS_ON(load_rules, -ENODEV, rules_dir);
}

int dmod_init(const Dmod_Config_t* Config)
{
    (void)Config;
    g_lock = dmosi_mutex_create(true);
    return (g_lock != NULL) ? 0 : -ENOMEM;
}

int dmod_deinit(void)
{
    lock();
    g_ready = false;
    libsystemd_report_t* report = report_pop();
    while (report != NULL)
    {
        report_destroy(report);
        report = report_pop();
    }
    unlock();
    if (g_lock != NULL)
    {
        dmosi_mutex_destroy(g_lock);
        g_lock = NULL;
    }
    return 0;
}
