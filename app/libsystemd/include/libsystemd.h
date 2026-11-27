#ifndef LIBSYSTEMD_H
#define LIBSYSTEMD_H

#include "dmod.h"
#include "libsystemd_defs.h"
#include "libsystemd_types.h"

dmod_libsystemd_api(1.0, int, _start_service, ( const char* unit_name, const char* user_value ));
dmod_libsystemd_api(1.0, int, _stop_service, ( const char* unit_name ));
dmod_libsystemd_api(1.0, int, _notify_main_pid, ( const char* unit_name, Dmod_Pid_t pid ));
dmod_libsystemd_api(1.0, int, _set_stop_semaphore, ( dmosi_semaphore_t wakeup ));
dmod_libsystemd_api(1.0, bool, _stop_requested, ( void ));
dmod_libsystemd_api(1.0, int, _status, ( const char* unit_name, libsystemd_service_status_t* out_status ));
dmod_libsystemd_api(1.0, int, _list, (libsystemd_visitor_t visitor, void* user_ptr));

dmod_libsystemd_api(1.0, int, _scan, (const char* path));
dmod_libsystemd_api(1.0, int, _parse_file, ( const char* file_path, libsystemd_service_t* service ));
dmod_libsystemd_api(1.0, int, _parse_dir, ( const char* dir_path, libsystemd_services_t* services ));

dmod_libsystemd_api(1.0, int, _load_rules, (const char* rules_dir));
dmod_libsystemd_api(1.0, int, _notify_device_added, (const char* device_class, const char* device_name, const char* user_value));
dmod_libsystemd_api(1.0, int, _notify_device_removed, (const char* device_class, const char* device_name));

/**
 * @brief Give the implementation what was reported before it was there
 *
 * libsystemd is loaded early and small. Until the module libsystemd_core (the units, the rules, the supervision) is loaded and
 * enabled, it keeps the reports of the devices and of the mounts and answers -ENODEV to the rest of the calls. The service
 * managers (systemd, service) call this when they start, once libsystemd_core is there: the kept reports are given to it, in
 * the order they came, through the dif below, and from then on every call is passed on to it. A call that comes first does the
 * same, so the order of the two does not matter.
 *
 * @return 0 when the kept reports were given (also when there were none), -ENODEV when libsystemd_core is not enabled yet,
 *         -EALREADY when they are being given just now
 */
dmod_libsystemd_api(1.0, int, _deliver_reports, (void));

/**
 * The dif libsystemd_core implements: the calls of the API above that need the units (the names are the ones of the API with
 * the prefix _impl). libsystemd finds the module that implements it when a call comes.
 */
dmod_libsystemd_dif(1.0, int,  _impl_start_service, ( const char* unit_name, const char* user_value ));
dmod_libsystemd_dif(1.0, int,  _impl_stop_service, ( const char* unit_name ));
dmod_libsystemd_dif(1.0, int,  _impl_notify_main_pid, ( const char* unit_name, Dmod_Pid_t pid ));
dmod_libsystemd_dif(1.0, int,  _impl_set_stop_semaphore, ( dmosi_semaphore_t wakeup ));
dmod_libsystemd_dif(1.0, bool, _impl_stop_requested, ( void ));
dmod_libsystemd_dif(1.0, int,  _impl_status, ( const char* unit_name, libsystemd_service_status_t* out_status ));
dmod_libsystemd_dif(1.0, int,  _impl_list, (libsystemd_visitor_t visitor, void* user_ptr));
dmod_libsystemd_dif(1.0, int,  _impl_scan, (const char* path));
dmod_libsystemd_dif(1.0, int,  _impl_parse_file, ( const char* file_path, libsystemd_service_t* service ));
dmod_libsystemd_dif(1.0, int,  _impl_parse_dir, ( const char* dir_path, libsystemd_services_t* services ));
dmod_libsystemd_dif(1.0, int,  _impl_load_rules, (const char* rules_dir));
dmod_libsystemd_dif(1.0, int,  _impl_notify_device_added, (const char* device_class, const char* device_name, const char* user_value));
dmod_libsystemd_dif(1.0, int,  _impl_notify_device_removed, (const char* device_class, const char* device_name));
dmod_libsystemd_dif(1.0, void, _impl_mount_changed, (const char* mount_point, bool mounted));

#endif // LIBSYSTEMD_H
