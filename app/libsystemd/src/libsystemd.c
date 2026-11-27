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
#include "dmlist.h"

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
static Dmod_Context_t*              g_impl          = NULL;     //!< the module that implements the dif (kept while it is valid and enabled)
static bool                         g_ready         = false;    //!< the implementation took the reports: calls are passed on
static bool                         g_delivering    = false;    //!< the reports are being given to the implementation
static dmlist_context_t*            g_reports       = NULL;     //!< the reports kept (libsystemd_report_t*), the oldest first

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
    return dmlist_pop_front(g_reports);
}

/** @brief Put the report at the end of the list (the lock held); it is destroyed when there is no room for it */
static bool report_push(libsystemd_report_t* report)
{
    if (!dmlist_push_back(g_reports, report))
    {
        report_destroy(report);
        return false;
    }
    return true;
}

/** @brief The same kind, class and name (the class is NULL for a mount) - what the list is searched by */
static int report_compare(const void* a, const void* b)
{
    const libsystemd_report_t* x = (const libsystemd_report_t*)a;
    const libsystemd_report_t* y = (const libsystemd_report_t*)b;
    if (x->kind != y->kind || strcmp(x->name, y->name) != 0)
    {
        return 1;
    }
    if (x->device_class == NULL || y->device_class == NULL)
    {
        return (x->device_class == y->device_class) ? 0 : 1;
    }
    return strcmp(x->device_class, y->device_class);
}

/** @brief Find the report of the device in the list (the lock held): the same kind, class and name */
static libsystemd_report_t* report_find(libsystemd_report_kind_t kind, const char* device_class, const char* name)
{
    libsystemd_report_t key = { .kind = kind, .device_class = (char*)device_class, .name = (char*)name };
    return dmlist_find(g_reports, &key, report_compare);
}

/** @brief Take the report out of the list and free it (the lock held) */
static void report_drop(libsystemd_report_t* report)
{
    dmlist_remove(g_reports, report, report_compare);
    report_destroy(report);
}

/**
 * @brief The function of the dif the implementation (libsystemd_core) offers, NULL when there is none
 *
 * The module that implements the dif is kept (g_impl): while its context is valid and the module is enabled, the function is
 * taken from it. The modules are searched again only when it is not (it was unloaded, or it was not there yet).
 */
static void* impl_function(const char* signature)
{
    lock();
    Dmod_Context_t* impl = g_impl;
    unlock();

    void* function = (impl != NULL) ? Dmod_GetDifFunction(impl, signature) : NULL;
    if (function == NULL)
    {
        impl = Dmod_GetNextDifModule(signature, NULL);
        function = (impl != NULL) ? Dmod_GetDifFunction(impl, signature) : NULL;
        lock();
        g_impl = impl;
        unlock();
    }
    return function;
}

static int deliver_reports(void);

/** @brief The same, but only once the implementation took the reports: before it the calls are kept (or refused) */
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
        // The implementation is gone: what is reported from now on is kept again
        lock();
        g_ready = false;
        unlock();
    }
    return function;
}

/**
 * @brief Put a report of a device on the list (the lock held)
 *
 * A device removed while the report of its adding is still kept cancels it: with no rules there was nothing to do for it,
 * and the implementation forgets such a device the same way.
 *
 * @return 0 kept (or dropped), -ENOMEM
 */
static int device_report_store(libsystemd_report_kind_t kind, const char* device_class, const char* name, const char* value)
{
    libsystemd_report_t* added = report_find(LIBSYSTEMD_REPORT_ADDED, device_class, name);
    if (kind == LIBSYSTEMD_REPORT_REMOVED && added != NULL)
    {
        report_drop(added);
        return 0;
    }
    if (kind == LIBSYSTEMD_REPORT_ADDED && added != NULL)
    {
        return 0;
    }

    if (dmlist_size(g_reports) >= LIBSYSTEMD_REPORTS_MAX)
    {
        DMOD_LOG_WARN("libsystemd: %u reports are kept and nothing takes them - '%s/%s' is lost\n", (unsigned)dmlist_size(g_reports), device_class, name);
        return -ENOMEM;
    }

    libsystemd_report_t* report = report_create(kind, device_class, name, value);
    return (report != NULL && report_push(report)) ? 0 : -ENOMEM;
}

/**
 * @brief Keep a report of a device for the implementation, and the answer for the caller
 *
 * The same answer as "no rule is loaded yet" (-ENOENT). When the implementation took the reports while this one was being kept,
 * it is given this one too, so it does not wait for the next time.
 */
static int device_report_keep(libsystemd_report_kind_t kind, const char* device_class, const char* name, const char* value)
{
    lock();
    int result = device_report_store(kind, device_class, name, value);
    bool ready = g_ready;
    unlock();

    if (ready)
    {
        deliver_reports();
    }
    return (result == 0) ? -ENOENT : result;
}

dmod_libsystemd_api_declaration(1.0, int, _notify_device_added, (const char* device_class, const char* device_name, const char* user_value))
{
    if (device_class == NULL || device_name == NULL)
    {
        return -EINVAL;
    }
    dmod_libsystemd_impl_notify_device_added_t function = ready_function(dmod_libsystemd_impl_notify_device_added_sig);
    if (function != NULL)
    {
        return function(device_class, device_name, user_value);
    }
    return device_report_keep(LIBSYSTEMD_REPORT_ADDED, device_class, device_name, user_value);
}

dmod_libsystemd_api_declaration(1.0, int, _notify_device_removed, (const char* device_class, const char* device_name))
{
    if (device_class == NULL || device_name == NULL)
    {
        return -EINVAL;
    }
    dmod_libsystemd_impl_notify_device_removed_t function = ready_function(dmod_libsystemd_impl_notify_device_removed_sig);
    if (function != NULL)
    {
        return function(device_class, device_name);
    }
    return device_report_keep(LIBSYSTEMD_REPORT_REMOVED, device_class, device_name, NULL);
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

    dmod_libsystemd_impl_mount_changed_t function = ready_function(dmod_libsystemd_impl_mount_changed_sig);
    if (function != NULL)
    {
        function(mount_point, mounted);
        return;
    }

    lock();
    libsystemd_report_t* report = NULL;
    if (dmlist_size(g_reports) < LIBSYSTEMD_REPORTS_MAX)
    {
        report = report_create(mounted ? LIBSYSTEMD_REPORT_MOUNTED : LIBSYSTEMD_REPORT_UNMOUNTED, NULL, mount_point, NULL);
    }
    if (report == NULL || !report_push(report))
    {
        DMOD_LOG_WARN("libsystemd: the %s of '%s' is lost (no room for the report)\n", mounted ? "mount" : "unmount", mount_point);
    }
    bool ready = g_ready;
    unlock();

    if (ready)
    {
        deliver_reports();
    }
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
    sink.added         = impl_function(dmod_libsystemd_impl_notify_device_added_sig);
    sink.removed       = impl_function(dmod_libsystemd_impl_notify_device_removed_sig);
    sink.mount_changed = impl_function(dmod_libsystemd_impl_mount_changed_sig);
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
    dmod_libsystemd_impl_start_service_t impl = ready_function(dmod_libsystemd_impl_start_service_sig);
    return (impl != NULL) ? impl(unit_name, user_value) : -ENODEV;
}

dmod_libsystemd_api_declaration(1.0, int, _stop_service, ( const char* unit_name ))
{
    dmod_libsystemd_impl_stop_service_t impl = ready_function(dmod_libsystemd_impl_stop_service_sig);
    return (impl != NULL) ? impl(unit_name) : -ENODEV;
}

dmod_libsystemd_api_declaration(1.0, int, _notify_main_pid, ( const char* unit_name, Dmod_Pid_t pid ))
{
    dmod_libsystemd_impl_notify_main_pid_t impl = ready_function(dmod_libsystemd_impl_notify_main_pid_sig);
    return (impl != NULL) ? impl(unit_name, pid) : -ENODEV;
}

dmod_libsystemd_api_declaration(1.0, int, _set_stop_semaphore, ( dmosi_semaphore_t wakeup ))
{
    dmod_libsystemd_impl_set_stop_semaphore_t impl = ready_function(dmod_libsystemd_impl_set_stop_semaphore_sig);
    return (impl != NULL) ? impl(wakeup) : -ENODEV;
}

dmod_libsystemd_api_declaration(1.0, bool, _stop_requested, ( void ))
{
    dmod_libsystemd_impl_stop_requested_t impl = ready_function(dmod_libsystemd_impl_stop_requested_sig);
    return (impl != NULL) ? impl() : false;
}

dmod_libsystemd_api_declaration(1.0, int, _status, ( const char* unit_name, libsystemd_service_status_t* out_status ))
{
    dmod_libsystemd_impl_status_t impl = ready_function(dmod_libsystemd_impl_status_sig);
    return (impl != NULL) ? impl(unit_name, out_status) : -ENODEV;
}

dmod_libsystemd_api_declaration(1.0, int, _list, (libsystemd_visitor_t visitor, void* user_ptr))
{
    dmod_libsystemd_impl_list_t impl = ready_function(dmod_libsystemd_impl_list_sig);
    return (impl != NULL) ? impl(visitor, user_ptr) : -ENODEV;
}

dmod_libsystemd_api_declaration(1.0, int, _scan, (const char* path))
{
    dmod_libsystemd_impl_scan_t impl = ready_function(dmod_libsystemd_impl_scan_sig);
    return (impl != NULL) ? impl(path) : -ENODEV;
}

dmod_libsystemd_api_declaration(1.0, int, _parse_file, ( const char* file_path, libsystemd_service_t* service ))
{
    dmod_libsystemd_impl_parse_file_t impl = ready_function(dmod_libsystemd_impl_parse_file_sig);
    return (impl != NULL) ? impl(file_path, service) : -ENODEV;
}

dmod_libsystemd_api_declaration(1.0, int, _parse_dir, ( const char* dir_path, libsystemd_services_t* services ))
{
    dmod_libsystemd_impl_parse_dir_t impl = ready_function(dmod_libsystemd_impl_parse_dir_sig);
    return (impl != NULL) ? impl(dir_path, services) : -ENODEV;
}

dmod_libsystemd_api_declaration(1.0, int, _load_rules, (const char* rules_dir))
{
    dmod_libsystemd_impl_load_rules_t impl = ready_function(dmod_libsystemd_impl_load_rules_sig);
    return (impl != NULL) ? impl(rules_dir) : -ENODEV;
}

int dmod_init(const Dmod_Config_t* Config)
{
    (void)Config;
    g_lock    = dmosi_mutex_create(true);
    g_reports = dmlist_create();
    return (g_lock != NULL && g_reports != NULL) ? 0 : -ENOMEM;
}

int dmod_deinit(void)
{
    lock();
    g_ready = false;
    g_impl  = NULL;
    libsystemd_report_t* report = report_pop();
    while (report != NULL)
    {
        report_destroy(report);
        report = report_pop();
    }
    dmlist_destroy(g_reports);
    g_reports = NULL;
    unlock();
    if (g_lock != NULL)
    {
        dmosi_mutex_destroy(g_lock);
        g_lock = NULL;
    }
    return 0;
}
