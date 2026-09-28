#include "dmod.h"
#include "dmosi.h"
#include "libsystemd.h"
#include <errno.h>
#include <string.h>

/*
 * Stand-in service for the graceful stop steps in libsystemd_test.c.
 *
 *   test_stoppable cooperative   registers for stop requests and leaves main()
 *                                when asked; returns TEST_STOPPABLE_EXIT_STATUS
 *   test_stoppable hung          registers, then hangs (blocks forever on a
 *                                semaphore nobody posts) and never looks at
 *                                the request
 *   test_stoppable plain         never registers (killed right away)
 */

/** Returned from main() when leaving on a stop request - distinguishes a clean exit from a kill (status 0). */
#define TEST_STOPPABLE_EXIT_STATUS  42

static int run_cooperative(dmosi_semaphore_t wakeup)
{
    libsystemd_set_stop_semaphore(wakeup);
    while (!libsystemd_stop_requested())
    {
        dmosi_semaphore_wait(wakeup, 1, 1000);
    }
    libsystemd_set_stop_semaphore(NULL);
    return TEST_STOPPABLE_EXIT_STATUS;
}

static int run_hung(dmosi_semaphore_t wakeup)
{
    dmosi_semaphore_t never = dmosi_semaphore_create(0, 1);
    libsystemd_set_stop_semaphore(wakeup);
    for (;;)
    {
        dmosi_semaphore_wait(never, 1, -1);
    }
    return 0;
}

int main(int argc, char* argv[])
{
    const char* mode = (argc > 1) ? argv[1] : "plain";
    dmosi_semaphore_t wakeup = dmosi_semaphore_create(0, 1);
    if (wakeup == NULL)
    {
        return -ENOMEM;
    }

    int status = 0;
    if (strcmp(mode, "cooperative") == 0)
    {
        status = run_cooperative(wakeup);
    }
    else if (strcmp(mode, "hung") == 0)
    {
        status = run_hung(wakeup);
    }
    else
    {
        for (;;)
        {
            dmosi_semaphore_wait(wakeup, 1, 1000);
        }
    }

    dmosi_semaphore_destroy(wakeup);
    return status;
}
