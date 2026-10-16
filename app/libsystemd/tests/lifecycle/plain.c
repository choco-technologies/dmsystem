#define DMOD_ENABLE_REGISTRATION ON
#include "dmod.h"
#include <errno.h>

static void* allocation;
int dmod_init(const Dmod_Config_t* config)
{
    (void)config;
    allocation = Dmod_Malloc(1);
    return allocation == NULL ? -ENOMEM : 0;
}
int dmod_deinit(void)
{
    Dmod_Free(allocation);
    allocation = NULL;
    return 0;
}
