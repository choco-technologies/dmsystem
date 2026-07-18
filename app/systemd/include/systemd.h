#ifndef SYSTEMD_H
#define SYSTEMD_H

#include "dmod.h"
#include "systemd_defs.h"
#include "systemd_types.h"

dmod_systemd_api(1.0, int, _start_service, ( const char* unit_name ));
dmod_systemd_api(1.0, int, _stop_service, ( const char* unit_name ));
dmod_systemd_api(1.0, int, _status, ( const char* unit_name, systemd_service_status_t* out_status ));

#endif // SYSTEMD_H