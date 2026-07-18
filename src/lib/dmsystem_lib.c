#define DMOD_ENABLE_REGISTRATION    ON
#include <dmsystem_lib.h>

/* ---- DMOD lifecycle ---- */

int dmod_init(const Dmod_Config_t *Config)
{
    DMOD_LOG_INFO("DMUART interface module initialized\n");
    return 0;
}

int dmod_deinit(void)
{
    DMOD_LOG_INFO("DMUART interface module deinitialized\n");
    return 0;
}
