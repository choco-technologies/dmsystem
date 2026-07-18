#include "systemd.h"
#include <errno.h>
#include "dmini.h"

struct systemd_service
{
    char* module_name;
    int argc;
    char** argv;
    Dmod_StreamRedirections_t streams;
    int starting_order;
    dmlist_context_t* required;
    dmlist_context_t* after;
};

struct systemd_services
{
    dmlist_context_t* services;
};

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

dmod_systemd_api_declaration(1.0, int, _scan, (const char* path))
{
    return -ENOSYS;
}

dmod_systemd_api_declaration(1.0, int, _parse_file, ( const char* file_path, systemd_service_t* service ))
{
    return NULL;
}

dmod_systemd_api_declaration(1.0, int, _parse_dir, ( const char* dir_path, systemd_services_t* services ))
{
    return NULL;
}


