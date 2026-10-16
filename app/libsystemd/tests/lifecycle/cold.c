#define DMOD_ENABLE_REGISTRATION ON
#include "dmod.h"
#include "libsystemd_service.h"
#include <errno.h>
#include <string.h>
static char* active;
dmod_libsystemd_dif_api_declaration(1.0, dmtestcold, int, _service_start,
    (const char* unit,int argc,const char* const argv[]))
{
    if(argc!=2 || strcmp(argv[1],"/cold/clock")!=0)return -EINVAL;
    if(active!=NULL)return -EALREADY;
    active=Dmod_Malloc(strlen(unit)+1);
    if(active==NULL)return -ENOMEM;
    strcpy(active,unit);return 0;
}
dmod_libsystemd_dif_api_declaration(1.0, dmtestcold, int, _service_stop, (const char* unit))
{
    if(active==NULL || strcmp(active,unit)!=0)return -ENOENT;
    Dmod_Free(active);active=NULL;return 0;
}
int dmod_init(const Dmod_Config_t* config) {(void)config;return 0;}
int dmod_deinit(void) {Dmod_Free(active);active=NULL;return 0;}
