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
    DMOD_TEST_EXPECT_EQ(libsystemd_start_service("does-not-exist", NULL), -ENOENT);
}

DMOD_TEST_STEP(start_service_rejects_null_unit_name)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_start_service(NULL, NULL), -EINVAL);
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

    /* Not asserting the registry is completely empty here: libsystemd_scan()
     * also replays any device reported (and still pending) from an earlier
     * test step in this same process (libsystemd_replay_pending_devices()) -
     * see notify_device_added_is_replayed_once_units_directory_is_scanned().
     * The actual invariant under test is that a bare template contributes
     * nothing *on its own* via the directory scan. */
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
    libsystemd_start_service("bare@one", NULL);

    DMOD_TEST_EXPECT_EQ(libsystemd_status("bare@one", &status), 0);

    unit_summary_t summary = { 0 };
    DMOD_TEST_EXPECT_EQ(libsystemd_list(count_units_visitor, &summary), 0);

    /* A second start_service() call finds the now-registered instance
     * directly - it must not be instantiated (or registered) twice. Compared
     * as a delta (not an absolute count): libsystemd_scan()'s device-replay
     * pass can have injected other "bare@<instance>" entries left pending by
     * earlier test steps in this same process (see
     * notify_device_added_is_replayed_once_units_directory_is_scanned()). */
    libsystemd_start_service("bare@one", NULL);
    unit_summary_t summary_again = { 0 };
    DMOD_TEST_EXPECT_EQ(libsystemd_list(count_units_visitor, &summary_again), 0);
    DMOD_TEST_EXPECT_EQ(summary_again.count, summary.count);
}

DMOD_TEST_STEP(start_service_rejects_instance_with_no_matching_template)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(LIBSYSTEMD_TEMPLATE_ONLY_FIXTURES_DIR), 0);

    /* "other@.ini" does not exist anywhere in the scanned directory. */
    DMOD_TEST_EXPECT_EQ(libsystemd_start_service("other@one", NULL), -ENOENT);
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

/**
 * Fixture directory: tests/fixtures/rules/, containing "devices.ini" with
 * "[class=tty] start=bare@%name" and "[class=net] start=other@%name". "bare"
 * matches the "bare@.ini" template in LIBSYSTEMD_TEMPLATE_ONLY_FIXTURES_DIR,
 * so scanning that as the units directory before loading these rules lets
 * "tty" devices actually resolve to an instantiable unit.
 */
#define LIBSYSTEMD_RULES_FIXTURES_DIR LIBSYSTEMD_TEST_FIXTURES_DIR "/rules"

DMOD_TEST_STEP(load_rules_rejects_null_path)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_load_rules(NULL), -EINVAL);
}

DMOD_TEST_STEP(load_rules_rejects_missing_directory)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_load_rules("/this/path/does/not/exist"), -ENOENT);
}

DMOD_TEST_STEP(load_rules_parses_class_sections)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_load_rules(LIBSYSTEMD_RULES_FIXTURES_DIR), 0);
}

DMOD_TEST_STEP(notify_device_added_rejects_null_arguments)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_notify_device_added(NULL, "tty1", NULL), -EINVAL);
    DMOD_TEST_EXPECT_EQ(libsystemd_notify_device_added("tty", NULL, NULL), -EINVAL);
}

DMOD_TEST_STEP(notify_device_removed_rejects_null_arguments)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_notify_device_removed(NULL, "tty1"), -EINVAL);
    DMOD_TEST_EXPECT_EQ(libsystemd_notify_device_removed("tty", NULL), -EINVAL);
}

DMOD_TEST_STEP(notify_device_added_starts_unit_matching_class_rule)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(LIBSYSTEMD_TEMPLATE_ONLY_FIXTURES_DIR), 0);
    DMOD_TEST_EXPECT_EQ(libsystemd_load_rules(LIBSYSTEMD_RULES_FIXTURES_DIR), 0);

    libsystemd_service_status_t status;
    DMOD_TEST_EXPECT_EQ(libsystemd_status("bare@tty1", &status), -ENOENT);

    /* "dmbare" isn't loadable in this test environment - what matters here
     * is that "tty"+"tty1" resolved through the rule to "bare@tty1" and
     * libsystemd_start_service() instantiated+registered it from the
     * template, exactly like a direct libsystemd_start_service("bare@tty1")
     * call would (see start_service_instantiates_template_on_demand). */
    libsystemd_notify_device_added("tty", "tty1", NULL);

    DMOD_TEST_EXPECT_EQ(libsystemd_status("bare@tty1", &status), 0);
}

DMOD_TEST_STEP(notify_device_added_rejects_unmatched_class)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_load_rules(LIBSYSTEMD_RULES_FIXTURES_DIR), 0);

    /* The rules fixture only defines "tty" and "net" classes. */
    DMOD_TEST_EXPECT_EQ(libsystemd_notify_device_added("usb", "sda", NULL), -ENOENT);
}

DMOD_TEST_STEP(notify_device_removed_stops_unit_matching_class_rule)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(LIBSYSTEMD_TEMPLATE_ONLY_FIXTURES_DIR), 0);
    DMOD_TEST_EXPECT_EQ(libsystemd_load_rules(LIBSYSTEMD_RULES_FIXTURES_DIR), 0);

    libsystemd_notify_device_added("tty", "tty1", NULL); /* registers "bare@tty1" */

    /* Never actually running ("dmbare" isn't loadable), so this mirrors
     * stop_service_reports_not_running_for_unspawnable_unit: the point is
     * that notify_device_removed() resolved the exact same target as
     * notify_device_added() did and reached libsystemd_stop_service(). */
    DMOD_TEST_EXPECT_EQ(libsystemd_notify_device_removed("tty", "tty1"), -ESRCH);
}

DMOD_TEST_STEP(notify_device_removed_rejects_unmatched_class)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_load_rules(LIBSYSTEMD_RULES_FIXTURES_DIR), 0);
    DMOD_TEST_EXPECT_EQ(libsystemd_notify_device_removed("usb", "sda"), -ENOENT);
}

/**
 * Fixture directories: tests/fixtures/rules_replay_{tty,scan,forget}/, each
 * with its own class ("replay-tty"/"replay-scan"/"replay-forget", all
 * mapping to "bare@%name") unique to one test each - deliberately *not*
 * shared, since libsystemd_load_rules() replaces g_rules wholesale and
 * g_rules/g_devices are process-global state shared across every test step
 * in this binary; one combined fixture would let one test's load_rules()
 * call silently satisfy another test's "no rule loaded yet" precondition.
 * These exercise the g_devices/libsystemd_replay_pending_devices()
 * machinery: a device reported via libsystemd_notify_device_added() before a
 * matching rule or the right units directory exists must still end up
 * started once libsystemd_load_rules()/libsystemd_scan() catches up - drivers
 * are typically loaded (and start reporting devices) before either one runs.
 */
#define LIBSYSTEMD_RULES_REPLAY_TTY_FIXTURES_DIR LIBSYSTEMD_TEST_FIXTURES_DIR "/rules_replay_tty"
#define LIBSYSTEMD_RULES_REPLAY_SCAN_FIXTURES_DIR LIBSYSTEMD_TEST_FIXTURES_DIR "/rules_replay_scan"
#define LIBSYSTEMD_RULES_REPLAY_FORGET_FIXTURES_DIR LIBSYSTEMD_TEST_FIXTURES_DIR "/rules_replay_forget"

DMOD_TEST_STEP(notify_device_added_is_replayed_once_matching_rules_are_loaded)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(LIBSYSTEMD_TEMPLATE_ONLY_FIXTURES_DIR), 0);

    /* No rule for "replay-tty" exists anywhere yet - remembered, not started. */
    DMOD_TEST_EXPECT_EQ(libsystemd_notify_device_added("replay-tty", "x1", NULL), -ENOENT);

    libsystemd_service_status_t status;
    DMOD_TEST_EXPECT_EQ(libsystemd_status("bare@x1", &status), -ENOENT);

    /* Loading the matching rules replays every still-pending device against
     * them - "replay-tty"+"x1" now resolves to "bare@x1" and gets started. */
    DMOD_TEST_EXPECT_EQ(libsystemd_load_rules(LIBSYSTEMD_RULES_REPLAY_TTY_FIXTURES_DIR), 0);

    DMOD_TEST_EXPECT_EQ(libsystemd_status("bare@x1", &status), 0);
}

DMOD_TEST_STEP(notify_device_added_is_replayed_once_units_directory_is_scanned)
{
    /* dmod_test_setup() has already scanned LIBSYSTEMD_EXAMPLES_DIR before
     * this step runs - it has no "bare@.ini" template, so resolving
     * "replay-scan" (which the rules below map to "bare@%name") finds a
     * matching rule but cannot instantiate a unit from it yet. */
    DMOD_TEST_EXPECT_EQ(libsystemd_load_rules(LIBSYSTEMD_RULES_REPLAY_SCAN_FIXTURES_DIR), 0);
    DMOD_TEST_EXPECT_NE(libsystemd_notify_device_added("replay-scan", "x2", NULL), 0);

    libsystemd_service_status_t status;
    DMOD_TEST_EXPECT_EQ(libsystemd_status("bare@x2", &status), -ENOENT);

    /* Scanning the units directory that actually has "bare@.ini" replays
     * every still-pending device - "replay-scan"+"x2" now instantiates and
     * starts successfully. */
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(LIBSYSTEMD_TEMPLATE_ONLY_FIXTURES_DIR), 0);

    DMOD_TEST_EXPECT_EQ(libsystemd_status("bare@x2", &status), 0);
}

/**
 * Fixture directory: tests/fixtures/metadata/, exercising the
 * "description"/"type"/"restart" ini keys added on top of the pre-existing
 * exec/args/after/requires/stdin/stdout/stderr/stdlog set - see
 * libsystemd_parse_service_type()/libsystemd_parse_restart_policy() in
 * serviceapi.c and app/libsystemd/docs/configuration.md#restart-supervision.
 */
#define LIBSYSTEMD_METADATA_FIXTURES_DIR LIBSYSTEMD_TEST_FIXTURES_DIR "/metadata"

/**
 * @brief Closure for find_info_visitor(), used by the metadata test steps below
 */
typedef struct
{
    const char* unit_name;
    bool found;
    libsystemd_service_info_t info;
} find_info_state_t;

static bool find_info_visitor(const libsystemd_service_info_t* info, void* user_ptr)
{
    find_info_state_t* state = (find_info_state_t*)user_ptr;

    if (strcmp(info->unit_name, state->unit_name) == 0)
    {
        state->info = *info;
        state->found = true;
        return false;
    }

    return true;
}

DMOD_TEST_STEP(scan_defaults_type_and_restart_when_keys_absent)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(LIBSYSTEMD_METADATA_FIXTURES_DIR), 0);

    find_info_state_t state = { .unit_name = "defaults", .found = false };
    DMOD_TEST_EXPECT_EQ(libsystemd_list(find_info_visitor, &state), 0);
    DMOD_TEST_EXPECT_TRUE(state.found);
    DMOD_TEST_EXPECT_NULL(state.info.description);
    DMOD_TEST_EXPECT_EQ(state.info.type, LIBSYSTEMD_SERVICE_TYPE_SIMPLE);
    DMOD_TEST_EXPECT_EQ(state.info.restart_policy, LIBSYSTEMD_RESTART_NO);
}

DMOD_TEST_STEP(scan_parses_oneshot_type_and_on_failure_restart)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(LIBSYSTEMD_METADATA_FIXTURES_DIR), 0);

    find_info_state_t state = { .unit_name = "oneshot", .found = false };
    DMOD_TEST_EXPECT_EQ(libsystemd_list(find_info_visitor, &state), 0);
    DMOD_TEST_EXPECT_TRUE(state.found);
    DMOD_TEST_EXPECT_NOT_NULL(state.info.description);
    DMOD_TEST_EXPECT_EQ(strcmp(state.info.description, "Runs once and exits"), 0);
    DMOD_TEST_EXPECT_EQ(state.info.type, LIBSYSTEMD_SERVICE_TYPE_ONESHOT);
    DMOD_TEST_EXPECT_EQ(state.info.restart_policy, LIBSYSTEMD_RESTART_ON_FAILURE);
}

DMOD_TEST_STEP(scan_parses_always_restart_policy)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(LIBSYSTEMD_METADATA_FIXTURES_DIR), 0);

    find_info_state_t state = { .unit_name = "always-restart", .found = false };
    DMOD_TEST_EXPECT_EQ(libsystemd_list(find_info_visitor, &state), 0);
    DMOD_TEST_EXPECT_TRUE(state.found);
    DMOD_TEST_EXPECT_EQ(state.info.restart_policy, LIBSYSTEMD_RESTART_ALWAYS);
}

DMOD_TEST_STEP(scan_falls_back_to_defaults_for_unrecognized_type_and_restart_values)
{
    /* "unknown-values.ini" sets type=forking and restart=on-success - neither
     * is a recognized value, so both must fall back to their defaults
     * (simple/no) rather than failing the parse or silently enabling restart
     * supervision. */
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(LIBSYSTEMD_METADATA_FIXTURES_DIR), 0);

    find_info_state_t state = { .unit_name = "unknown-values", .found = false };
    DMOD_TEST_EXPECT_EQ(libsystemd_list(find_info_visitor, &state), 0);
    DMOD_TEST_EXPECT_TRUE(state.found);
    DMOD_TEST_EXPECT_EQ(state.info.type, LIBSYSTEMD_SERVICE_TYPE_SIMPLE);
    DMOD_TEST_EXPECT_EQ(state.info.restart_policy, LIBSYSTEMD_RESTART_NO);
}

DMOD_TEST_STEP(notify_device_removed_forgets_a_pending_device)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(LIBSYSTEMD_TEMPLATE_ONLY_FIXTURES_DIR), 0);

    /* No rule for "replay-forget" exists yet - remembered. */
    DMOD_TEST_EXPECT_EQ(libsystemd_notify_device_added("replay-forget", "y1", NULL), -ENOENT);

    /* Removed again before any matching rule was ever loaded - forgotten,
     * so it must not be resurrected by the load_rules() below. */
    DMOD_TEST_EXPECT_EQ(libsystemd_notify_device_removed("replay-forget", "y1"), -ENOENT);

    DMOD_TEST_EXPECT_EQ(libsystemd_load_rules(LIBSYSTEMD_RULES_REPLAY_FORGET_FIXTURES_DIR), 0);

    libsystemd_service_status_t status;
    DMOD_TEST_EXPECT_EQ(libsystemd_status("bare@y1", &status), -ENOENT);
}

/**
 * The following steps exercise the `%v` specifier ("bare@.ini" sets
 * "description=%v", see tests/fixtures/template_only/bare@.ini) - the
 * caller-supplied value threaded through libsystemd_start_service()'s and
 * libsystemd_notify_device_added()'s optional last argument, all the way to
 * on-demand template instantiation (libsystemd_instantiate_from_template()).
 */

DMOD_TEST_STEP(start_service_substitutes_user_value_specifier_when_provided)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(LIBSYSTEMD_TEMPLATE_ONLY_FIXTURES_DIR), 0);

    /* "dmbare" isn't loadable in this test environment (see
     * start_service_instantiates_template_on_demand) - what matters here is
     * that the instance was still resolved from the template with "%v"
     * substituted for the given user_value. */
    libsystemd_start_service("bare@withvalue", "/dev/ttyS9");

    find_info_state_t state = { .unit_name = "bare@withvalue", .found = false };
    DMOD_TEST_EXPECT_EQ(libsystemd_list(find_info_visitor, &state), 0);
    DMOD_TEST_EXPECT_TRUE(state.found);
    DMOD_TEST_EXPECT_NOT_NULL(state.info.description);
    DMOD_TEST_EXPECT_EQ(strcmp(state.info.description, "/dev/ttyS9"), 0);
}

DMOD_TEST_STEP(start_service_expands_user_value_specifier_to_empty_when_omitted)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(LIBSYSTEMD_TEMPLATE_ONLY_FIXTURES_DIR), 0);

    libsystemd_start_service("bare@novalue", NULL);

    find_info_state_t state = { .unit_name = "bare@novalue", .found = false };
    DMOD_TEST_EXPECT_EQ(libsystemd_list(find_info_visitor, &state), 0);
    DMOD_TEST_EXPECT_TRUE(state.found);
    DMOD_TEST_EXPECT_NOT_NULL(state.info.description);
    DMOD_TEST_EXPECT_EQ(strcmp(state.info.description, ""), 0);
}

DMOD_TEST_STEP(notify_device_added_substitutes_user_value_specifier)
{
    DMOD_TEST_EXPECT_EQ(libsystemd_scan(LIBSYSTEMD_TEMPLATE_ONLY_FIXTURES_DIR), 0);
    DMOD_TEST_EXPECT_EQ(libsystemd_load_rules(LIBSYSTEMD_RULES_FIXTURES_DIR), 0);

    /* [class=tty] start=bare@%name (see LIBSYSTEMD_RULES_FIXTURES_DIR) resolves
     * "tty"+"path1" to "bare@path1", instantiated from "bare@.ini" with the
     * given user_value substituted for its "description=%v" key. */
    libsystemd_notify_device_added("tty", "path1", "/dev/ttyPATH1");

    find_info_state_t state = { .unit_name = "bare@path1", .found = false };
    DMOD_TEST_EXPECT_EQ(libsystemd_list(find_info_visitor, &state), 0);
    DMOD_TEST_EXPECT_TRUE(state.found);
    DMOD_TEST_EXPECT_NOT_NULL(state.info.description);
    DMOD_TEST_EXPECT_EQ(strcmp(state.info.description, "/dev/ttyPATH1"), 0);
}
