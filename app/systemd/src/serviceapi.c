#include "systemd.h"
#include <errno.h>

dmod_systemd_api_declaration(1.0, int, _start_service, ( const char* unit_name ))
{
    return -ENOSYS;
}

dmod_systemd_api_declaration(1.0, int, _stop_service, ( const char* unit_name ))
{
    return -ENOSYS;
}

dmod_systemd_api_declaration(1.0, int, _status, ( const char* unit_name, systemd_service_status_t* out_status ))
{
    return -ENOSYS;
}

dmod_systemd_api_declaration(1.0, int, _list, (systemd_visitor_t visitor, void* user_ptr))
{
    return -ENOSYS;
}

