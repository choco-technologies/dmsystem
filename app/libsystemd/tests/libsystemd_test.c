#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "libsystemd.h"
#include <errno.h>
#include <string.h>

#ifndef LIBSYSTEMD_EXAMPLES_DIR
#define LIBSYSTEMD_EXAMPLES_DIR "../examples"
#endif

#ifndef LIBSYSTEMD_TEST_FIXTURES_DIR
#define LIBSYSTEMD_TEST_FIXTURES_DIR "fixtures"
#endif

/**
 * @brief Runs before every DMOD_TEST_STEP - (re)scans the shipped example
 *        units so every step can assume the registry is populated,
 *        regardless of the order test steps happen to run in.
 */
void dmod_test_setup(void)
{
    libsystemd_scan(LIBSYSTEMD_EXAMPLES_DIR);
}

/**
 * @brief Closure used by count_units_visitor() to summarize a libsystemd_list() pass
 */
typedef struct
{
    int count;
    bool found_networking;
    bool found_webserver;
    bool found_monitoring;
} unit_summary_t;

static bool count_units_visitor(const libsystemd_service_info_t* info, void* user_ptr)
{
    unit_summary_t* summary = (unit_summary_t*)user_ptr;

    summary->count++;
    if (strcmp(info->unit_name, "networking") == 0)
    {
        summary->found_networking = true;
    }
    else if (strcmp(info->unit_name, "webserver") == 0)
    {
        summary->found_webserver = true;
    }
    else if (strcmp(info->unit_name, "monitoring") == 0)
    {
        summary->found_monitoring = true;
    }

    return true;
}

static bool stop_after_first_visitor(const libsystemd_service_info_t* info, void* user_ptr)
{
    (void)info;
    int* visits = (int*)user_ptr;
    (*visits)++;
    return false;
}

DMOD_TEST_STEP(parse_file_rejects_null_arguments)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_parse_file(NULL, NULL), -EINVAL);

    libsystemd_service_t service = NULL;
    DMOD_TEST_EXPECT_EQ(libsystemd_parse_file(NULL, &service), -EINVAL);
    DMOD_TEST_EXPECT_EQ(libsystemd_parse_file(LIBSYSTEMD_EXAMPLES_DIR "/networking.ini", NULL), -EINVAL);
}

DMOD_TEST_STEP(parse_file_rejects_missing_file)
{
    libsystemd_service_t service = NULL;
    int result = libsystemd_parse_file(LIBSYSTEMD_EXAMPLES_DIR "/does-not-exist.ini", &service);
    DMOD_TEST_EXPECT(result < 0);
    DMOD_TEST_EXPECT_NULL(service);
}

DMOD_TEST_STEP(parse_file_rejects_unit_without_exec_key)
{
    libsystemd_service_t service = NULL;
    int result = libsystemd_parse_file(LIBSYSTEMD_TEST_FIXTURES_DIR "/invalid-no-exec.ini", &service);
    DMOD_TEST_EXPECT_EQ(result, -EINVAL);
    DMOD_TEST_EXPECT_NULL(service);
}

DMOD_TEST_STEP(parse_dir_rejects_null_arguments)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_parse_dir(NULL, NULL), -EINVAL);

    libsystemd_services_t services = NULL;
    DMOD_TEST_EXPECT_EQ(libsystemd_parse_dir(NULL, &services), -EINVAL);
    DMOD_TEST_EXPECT_EQ(libsystemd_parse_dir(LIBSYSTEMD_EXAMPLES_DIR, NULL), -EINVAL);
}

DMOD_TEST_STEP(parse_dir_rejects_missing_directory)
{
    libsystemd_services_t services = NULL;
    DMOD_TEST_EXPECT_EQ(libsystemd_parse_dir("/this/path/does/not/exist", &services), -ENOENT);
}

DMOD_TEST_STEP(scan_rejects_null_path)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(NULL), -EINVAL);
}

DMOD_TEST_STEP(scan_rejects_missing_directory)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_scan("/this/path/does/not/exist"), -ENOENT);
}

DMOD_TEST_STEP(scan_parses_example_units)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(LIBSYSTEMD_EXAMPLES_DIR), 0);
}

DMOD_TEST_STEP(list_reports_all_example_units)
{
    unit_summary_t summary = { 0 };
    DMOD_TEST_EXPECT_EQ(libsystemd_list(count_units_visitor, &summary), 0);

    /* networking, webserver, monitoring, plus getty@tty1/getty@tty2 (two
     * instances of the getty@.ini template - the bare template itself is
     * never started, so it does not count towards this total). */
    DMOD_TEST_EXPECT_EQ(summary.count, 5);
    DMOD_TEST_EXPECT_TRUE(summary.found_networking);
    DMOD_TEST_EXPECT_TRUE(summary.found_webserver);
    DMOD_TEST_EXPECT_TRUE(summary.found_monitoring);
}

DMOD_TEST_STEP(list_rejects_null_visitor)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_list(NULL, NULL), -EINVAL);
}

DMOD_TEST_STEP(list_stops_early_when_visitor_returns_false)
{
    int visits = 0;
    DMOD_TEST_EXPECT_EQ(libsystemd_list(stop_after_first_visitor, &visits), 0);
    DMOD_TEST_EXPECT_EQ(visits, 1);
}

DMOD_TEST_STEP(status_rejects_unknown_unit)
{
    libsystemd_service_status_t status;
    DMOD_TEST_EXPECT_EQ(libsystemd_status("does-not-exist", &status), -ENOENT);
}

DMOD_TEST_STEP(status_rejects_null_arguments)
{
    libsystemd_service_status_t status;
    DMOD_TEST_EXPECT_EQ(libsystemd_status(NULL, &status), -EINVAL);
    DMOD_TEST_EXPECT_EQ(libsystemd_status("networking", NULL), -EINVAL);
}

DMOD_TEST_STEP(status_reports_known_unit)
{
    libsystemd_service_status_t status;
    DMOD_TEST_EXPECT_EQ(libsystemd_status("networking", &status), 0);
}

DMOD_TEST_STEP(start_service_rejects_unknown_unit)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_start_service("does-not-exist"), -ENOENT);
}

DMOD_TEST_STEP(start_service_rejects_null_unit_name)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_start_service(NULL), -EINVAL);
}

DMOD_TEST_STEP(stop_service_rejects_unknown_unit)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_stop_service("does-not-exist"), -ENOENT);
}

DMOD_TEST_STEP(stop_service_rejects_null_unit_name)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_stop_service(NULL), -EINVAL);
}

DMOD_TEST_STEP(stop_service_reports_not_running_for_unspawnable_unit)
{
    /* "networking"'s exec (dmnetd) is a placeholder module name from the
     * example unit file and is never actually loadable in this environment,
     * so libsystemd_scan()'s best-effort auto-start always leaves it stopped. */
    DMOD_TEST_EXPECT_EQ(libsystemd_stop_service("networking"), -ESRCH);
}

/**
 * Fixture directory for these steps: tests/fixtures/templates/, containing
 * a template "app@.ini" (exec=dmapp, args="--name %i --literal %%") and two
 * instances: "app@one.ini" (empty - fully inherited) and "app@two.ini"
 * (overrides "args" but still inherits "exec"/"description").
 */
#define LIBSYSTEMD_TEMPLATE_FIXTURES_DIR LIBSYSTEMD_TEST_FIXTURES_DIR "/templates"
#define LIBSYSTEMD_TEMPLATE_ONLY_FIXTURES_DIR LIBSYSTEMD_TEST_FIXTURES_DIR "/template_only"

DMOD_TEST_STEP(scan_expands_template_instances)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(LIBSYSTEMD_TEMPLATE_FIXTURES_DIR), 0);

    libsystemd_service_status_t status;

    /* Both instances were found, named "<prefix>@<instance>", and parsed
     * successfully (a failure to inherit "exec" from the template would
     * have made libsystemd_parse_file() reject them with -EINVAL, and they
     * would not be in the registry at all). */
    DMOD_TEST_EXPECT_EQ(libsystemd_status("app@one", &status), 0);
    DMOD_TEST_EXPECT_EQ(libsystemd_status("app@two", &status), 0);

    /* Neither the bare template nor the un-instantiated prefix is ever
     * registered as a startable unit. */
    DMOD_TEST_EXPECT_EQ(libsystemd_status("app@", &status), -ENOENT);
    DMOD_TEST_EXPECT_EQ(libsystemd_status("app", &status), -ENOENT);
}

DMOD_TEST_STEP(scan_lists_exactly_the_two_template_instances)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(LIBSYSTEMD_TEMPLATE_FIXTURES_DIR), 0);

    unit_summary_t summary = { 0 };
    DMOD_TEST_EXPECT_EQ(libsystemd_list(count_units_visitor, &summary), 0);
    DMOD_TEST_EXPECT_EQ(summary.count, 2);
}

DMOD_TEST_STEP(scan_does_not_start_a_bare_template_without_instances)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(LIBSYSTEMD_TEMPLATE_ONLY_FIXTURES_DIR), 0);

    unit_summary_t summary = { 0 };
    DMOD_TEST_EXPECT_EQ(libsystemd_list(count_units_visitor, &summary), 0);
    DMOD_TEST_EXPECT_EQ(summary.count, 0);

    libsystemd_service_status_t status;
    DMOD_TEST_EXPECT_EQ(libsystemd_status("bare@", &status), -ENOENT);
}

/**
 * Fixture directory: tests/fixtures/template_only/, containing only
 * "bare@.ini" (exec=dmbare) - no instance file on disk anywhere. These steps
 * exercise libsystemd_start_service()'s on-demand instantiation
 * (libsystemd_instantiate_service_on_demand()): starting "bare@one" (never
 * scanned, no file for it) should still create and register it purely from
 * the template, the same way `systemctl start foo@bar` would in real systemd.
 */
DMOD_TEST_STEP(start_service_instantiates_template_on_demand)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(LIBSYSTEMD_TEMPLATE_ONLY_FIXTURES_DIR), 0);

    libsystemd_service_status_t status;
    DMOD_TEST_EXPECT_EQ(libsystemd_status("bare@one", &status), -ENOENT);

    /* "dmbare" is not a loadable module in this test environment, so the
     * spawn itself fails - what this step checks is that the instance was
     * still resolved from the template and registered, exactly like a unit
     * whose exec module can't be found at libsystemd_scan() time still ends
     * up in the registry (see stop_service_reports_not_running_for_unspawnable_unit). */
    libsystemd_start_service("bare@one");

    DMOD_TEST_EXPECT_EQ(libsystemd_status("bare@one", &status), 0);

    unit_summary_t summary = { 0 };
    DMOD_TEST_EXPECT_EQ(libsystemd_list(count_units_visitor, &summary), 0);
    DMOD_TEST_EXPECT_EQ(summary.count, 1);

    /* A second start_service() call finds the now-registered instance
     * directly - it must not be instantiated (or registered) twice. */
    libsystemd_start_service("bare@one");
    unit_summary_t summary_again = { 0 };
    DMOD_TEST_EXPECT_EQ(libsystemd_list(count_units_visitor, &summary_again), 0);
    DMOD_TEST_EXPECT_EQ(summary_again.count, 1);
}

DMOD_TEST_STEP(start_service_rejects_instance_with_no_matching_template)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(LIBSYSTEMD_TEMPLATE_ONLY_FIXTURES_DIR), 0);

    /* "other@.ini" does not exist anywhere in the scanned directory. */
    DMOD_TEST_EXPECT_EQ(libsystemd_start_service("other@one"), -ENOENT);
}

/**
 * Regression fixture for libsystemd_build_streams(): "all-streams.ini" sets
 * all four of stdin/stdout/stderr/stdlog at once. Its entries array must be
 * sized for 4 candidates, not 3 - a unit setting all four used to overflow
 * the allocated array by one `Dmod_StreamRedirection_t` entry.
 */
#define LIBSYSTEMD_STREAMS_FIXTURES_DIR LIBSYSTEMD_TEST_FIXTURES_DIR "/streams"

DMOD_TEST_STEP(scan_parses_unit_with_all_four_stream_keys)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(LIBSYSTEMD_STREAMS_FIXTURES_DIR), 0);

    libsystemd_service_status_t status;
    DMOD_TEST_EXPECT_EQ(libsystemd_status("all-streams", &status), 0);
}
