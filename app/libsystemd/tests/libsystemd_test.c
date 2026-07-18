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

    DMOD_TEST_EXPECT_EQ(summary.count, 3);
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
