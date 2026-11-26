#define ENABLE_DIF_REGISTRATIONS    ON
#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "libsystemd.h"
#include "dmfsi.h"
#include <errno.h>
#include <string.h>

#ifndef LIBSYSTEMD_TEST_FIXTURES_DIR
#define LIBSYSTEMD_TEST_FIXTURES_DIR "../fixtures"
#endif

#define TEMPLATE_ONLY_DIR   LIBSYSTEMD_TEST_FIXTURES_DIR "/template_only"
#define RULES_EARLY_DIR     LIBSYSTEMD_TEST_FIXTURES_DIR "/rules_replay_early"

/**
 * One sequence, because it is the order that matters: the state of libsystemd (no core, then the core) is the one of
 * the whole process, so it cannot be split into steps that could run in any order.
 */
DMOD_TEST_STEP(front_keeps_reports_until_the_core_takes_them)
{
    libsystemd_service_status_t status;

    // Nothing implements the API yet: the calls that need the units say so
    DMOD_TEST_EXPECT_FALSE(Dmod_IsModuleEnabled("libsystemd_core"));
    DMOD_TEST_EXPECT_EQ(libsystemd_status("bare@x1", &status), -ENODEV);
    DMOD_TEST_EXPECT_EQ(libsystemd_start_service("bare@x1", NULL), -ENODEV);
    DMOD_TEST_EXPECT_EQ(libsystemd_stop_service("bare@x1"), -ENODEV);
    DMOD_TEST_EXPECT_EQ(libsystemd_list(NULL, NULL), -ENODEV);
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(TEMPLATE_ONLY_DIR), -ENODEV);
    DMOD_TEST_EXPECT_FALSE(libsystemd_stop_requested());

    // The reports are kept ("no rule yet", as the implementation answers it) - and checked as before
    DMOD_TEST_EXPECT_EQ(libsystemd_notify_device_added(NULL, "x1", NULL), -EINVAL);
    DMOD_TEST_EXPECT_EQ(libsystemd_notify_device_added("replay-early", "x1", NULL), -ENOENT);
    DMOD_TEST_EXPECT_EQ(libsystemd_notify_device_added("replay-early", "x1", NULL), -ENOENT);   // the same device again: kept once

    // A device that went away before anybody could do anything for it is not given to the implementation
    DMOD_TEST_EXPECT_EQ(libsystemd_notify_device_added("replay-early", "gone", NULL), -ENOENT);
    DMOD_TEST_EXPECT_EQ(libsystemd_notify_device_removed("replay-early", "gone"), -ENOENT);

    // The mount (the dif dmvfs calls): kept as well
    dmod_dmfsi_mount_changed_t mount_changed =
        (dmod_dmfsi_mount_changed_t)Dmod_GetDifFunction(Dmod_GetModuleContext("libsystemd"), dmod_dmfsi_mount_changed_sig);
    DMOD_TEST_EXPECT_NOT_NULL(mount_changed);
    if (mount_changed != NULL)
    {
        mount_changed("/early", true);
    }

    // The implementation is loaded: it gets what was kept, and the calls are served
    DMOD_TEST_EXPECT_NOT_NULL(Dmod_LoadModuleByName("libsystemd_core"));
    DMOD_TEST_EXPECT_TRUE(Dmod_EnableModule("libsystemd_core", false, NULL));
    DMOD_TEST_EXPECT_EQ(libsystemd_status("bare@x1", &status), -ENOENT);

    DMOD_TEST_EXPECT_EQ(libsystemd_scan(TEMPLATE_ONLY_DIR), 0);
    DMOD_TEST_EXPECT_EQ(libsystemd_status("bare@x1", &status), -ENOENT);     // no rule yet

    // The rules come: the device that was kept is started, the one that went away is not
    DMOD_TEST_EXPECT_EQ(libsystemd_load_rules(RULES_EARLY_DIR), 0);
    DMOD_TEST_EXPECT_EQ(libsystemd_status("bare@x1", &status), 0);
    DMOD_TEST_EXPECT_EQ(libsystemd_status("bare@gone", &status), -ENOENT);

    // From now on the calls are passed on at once
    libsystemd_notify_device_added("replay-early", "x3", NULL);     // (the unit's module is not there: the result is its start failing)
    DMOD_TEST_EXPECT_EQ(libsystemd_status("bare@x3", &status), 0);
}
