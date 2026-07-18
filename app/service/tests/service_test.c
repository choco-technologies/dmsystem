#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "libsystemd.h"
#include <errno.h>

/**
 * @brief Sanity check that this test module links against libsystemd's API successfully
 *
 * `service` itself is a thin CLI (subcommand dispatch over libsystemd's
 * control API); the substantial logic it calls into is already covered
 * end-to-end by libsystemd's own test suite
 * (app/libsystemd/tests/libsystemd_test.c). This simply confirms the
 * build/link wiring against `libsystemd` is intact from this module too.
 */
DMOD_TEST_STEP(links_against_libsystemd_api)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_status(NULL, NULL), -EINVAL);
}
