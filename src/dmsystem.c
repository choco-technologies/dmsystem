#include "dmod.h"
#include "dmsystem_core.h"
#include <errno.h>

/**
 * @brief Pre-initialization function for the module.
 *
 * Called while the module is still being enabled - just used here to confirm
 * that the dmsystem_core module dmsystem depends on is actually connected.
 */
void dmod_preinit(void)
{
    if (Dmod_IsFunctionConnected((void*)dmsystem_core_run))
    {
        Dmod_Printf("dmsystem: dmsystem_core API is connected\n");
    }
}

/**
 * @brief Entry point: scans the unit directory given as argv[1] and runs it
 *
 * @param argc Number of arguments
 * @param argv argv[1] must be the path to the directory containing unit (*.ini) files
 *
 * @return 0 if every started unit succeeded, a positive count of failed units,
 *         or a negative errno-style code if the units directory could not be opened
 */
int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        DMOD_LOG_ERROR("Usage: dmsystem <path-to-units-directory>\n");
        return -EINVAL;
    }

    return dmsystem_core_run(argv[1]);
}
