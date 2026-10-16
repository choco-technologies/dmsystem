#define DMOD_ENABLE_REGISTRATION ON
#include "dmod.h"
#include "dmtestlibrary.h"
#include "libsystemd_service.h"
#include <errno.h>
#include <string.h>
struct instance {char unit[96]; char path[128];};
static struct instance* instances[4];
dmod_dmtestlibrary_api_declaration(1.0, bool, _active, (const char* unit))
{
    for(unsigned i=0;i<4;++i) if(instances[i]!=NULL && strcmp(instances[i]->unit,unit)==0)return true;
    return false;
}
dmod_dmtestlibrary_api_declaration(1.0, int, _path, (const char* unit,char* path,size_t capacity))
{
    for(unsigned i=0;i<4;++i) if(instances[i]!=NULL && strcmp(instances[i]->unit,unit)==0) {
        if(strlen(instances[i]->path)+1>capacity)return -ENOSPC;
        strcpy(path,instances[i]->path); return 0;
    }
    return -ENOENT;
}
dmod_libsystemd_dif_api_declaration(1.0, dmtestlibrary, int, _service_start,
    (const char* unit,int argc,const char* const argv[]))
{
    if(argc!=2 || argv==NULL || strcmp(argv[1],"/reject")==0)return -EINVAL;
    if(strlen(unit)>=sizeof(((struct instance*)0)->unit) || strlen(argv[1])>=sizeof(((struct instance*)0)->path))return -ENAMETOOLONG;
    for(unsigned i=0;i<4;++i) if(instances[i]==NULL) {
        instances[i]=Dmod_Malloc(sizeof(*instances[i]));
        if(instances[i]==NULL)return -ENOMEM;
        strcpy(instances[i]->unit,unit); strcpy(instances[i]->path,argv[1]); return 0;
    }
    return -ENOSPC;
}
dmod_libsystemd_dif_api_declaration(1.0, dmtestlibrary, int, _service_stop, (const char* unit))
{
    for(unsigned i=0;i<4;++i) if(instances[i]!=NULL && strcmp(instances[i]->unit,unit)==0) {
        Dmod_Free(instances[i]);instances[i]=NULL; return 0;
    }
    return -ENOENT;
}
int dmod_init(const Dmod_Config_t* config) {(void)config;memset(instances,0,sizeof(instances));return 0;}
int dmod_deinit(void) {for(unsigned i=0;i<4;++i)Dmod_Free(instances[i]);return 0;}
