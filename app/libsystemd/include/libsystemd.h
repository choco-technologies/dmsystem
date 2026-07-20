#ifndef LIBSYSTEMD_H
#define LIBSYSTEMD_H

#include "dmod.h"
#include "libsystemd_defs.h"
#include "libsystemd_types.h"
#include "dmlist.h"

dmod_libsystemd_api(1.0, int, _start_service, ( const char* unit_name, const char* user_value ));
dmod_libsystemd_api(1.0, int, _stop_service, ( const char* unit_name ));
dmod_libsystemd_api(1.0, int, _status, ( const char* unit_name, libsystemd_service_status_t* out_status ));
dmod_libsystemd_api(1.0, int, _list, (libsystemd_visitor_t visitor, void* user_ptr));

dmod_libsystemd_api(1.0, int, _scan, (const char* path));
dmod_libsystemd_api(1.0, int, _parse_file, ( const char* file_path, libsystemd_service_t* service ));
dmod_libsystemd_api(1.0, int, _parse_dir, ( const char* dir_path, libsystemd_services_t* services ));

dmod_libsystemd_api(1.0, int, _load_rules, (const char* rules_dir));
dmod_libsystemd_api(1.0, int, _notify_device_added, (const char* device_class, const char* device_name, const char* user_value));
dmod_libsystemd_api(1.0, int, _notify_device_removed, (const char* device_class, const char* device_name));

#endif // LIBSYSTEMD_H
