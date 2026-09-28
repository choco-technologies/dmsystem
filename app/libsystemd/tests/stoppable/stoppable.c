#include "dmod.h"
#include "dmosi.h"
#include "libsystemd.h"
#include <errno.h>
#include <string.h>

/*
 * Stand-in service for the graceful stop steps in libsystemd_test.c.
 *
 *   test_stoppable cooperative   registers a stop handler that wakes the main
 *                                loop; returns TEST_STOPPABLE_EXIT_STATUS
 *   test_stoppable stubborn      registers a handler that ignores the request
 *   test_stoppable plain         registers nothing (killed right away)
 */

/** Returned from main() when leaving on a stop request - distinguishes a clean exit from a kill (status 0). */
#define TEST_STOPPABLE_EXIT_STATUS  42

typedef struct
{
    dmosi_semaphore_t wakeup;
    volatile bool stop;
} stoppable_t;

static void on_stop(void* arg)
{
    stoppable_t* s = (stoppable_t*)arg;
    s->stop = true;
    dmosi_semaphore_post(s->wakeup, 1);
}

static void ignore_stop(void* arg)
{
    (void)arg;
}

int main(int argc, char* argv[])
{
    const char* mode = (argc > 1) ? argv[1] : "plain";
    stoppable_t s = { .wakeup = dmosi_semaphore_create(0, 1), .stop = false };
    if (s.wakeup == NULL)
    {
        return -ENOMEM;
    }

    if (strcmp(mode, "cooperative") == 0)
    {
        libsystemd_set_stop_handler(on_stop, &s);
    }
    else if (strcmp(mode, "stubborn") == 0)
    {
        libsystemd_set_stop_handler(ignore_stop, &s);
    }

    while (!s.stop)
    {
        dmosi_semaphore_wait(s.wakeup, 1, 1000);
    }

    libsystemd_set_stop_handler(NULL, NULL);
    dmosi_semaphore_destroy(s.wakeup);
    return TEST_STOPPABLE_EXIT_STATUS;
}
