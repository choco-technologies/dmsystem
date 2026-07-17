/**
 * @file dmsystem_test.c
 * @brief Unit tests for the pure unit-model and dependency-ordering logic.
 *
 * Deliberately does not touch dmsystem_config.c/dmsystem_proc.c (the two files
 * that call into dmini/dmosi) - see tests/CMakeLists.txt for why.
 */
#include "dmod_test.h"
#include "dmsystem_unit.h"
#include "dmsystem_graph.h"
#include <string.h>

// ===============================================================
//                  Unit list management
// ===============================================================

DMOD_TEST_STEP(unit_list_add_and_find)
{
    dmsystem_unit_list_t list;
    dmsystem_unit_list_init(&list);

    dmsystem_unit_t* unit = dmsystem_unit_list_add(&list, "networking");
    DMOD_TEST_EXPECT_NOT_NULL(unit);
    DMOD_TEST_EXPECT_EQ(list.count, 1u);
    DMOD_TEST_EXPECT(strcmp(unit->name, "networking") == 0);
    DMOD_TEST_EXPECT_EQ((int)unit->state, (int)DMSYSTEM_UNIT_STATE_PENDING);

    dmsystem_unit_t* found = dmsystem_unit_list_find(&list, "networking");
    DMOD_TEST_EXPECT(found == unit);

    DMOD_TEST_EXPECT_NULL(dmsystem_unit_list_find(&list, "does-not-exist"));
}

DMOD_TEST_STEP(unit_list_respects_capacity)
{
    dmsystem_unit_list_t list;
    dmsystem_unit_list_init(&list);

    for (size_t i = 0; i < DMSYSTEM_MAX_UNITS; i++)
    {
        char name[16];
        Dmod_SnPrintf(name, sizeof(name), "unit%zu", i);
        DMOD_TEST_EXPECT_NOT_NULL(dmsystem_unit_list_add(&list, name));
    }

    DMOD_TEST_EXPECT_EQ(list.count, (size_t)DMSYSTEM_MAX_UNITS);
    DMOD_TEST_EXPECT_NULL(dmsystem_unit_list_add(&list, "one-too-many"));
}

// ===============================================================
//                  argv / dependency-list parsing
// ===============================================================

DMOD_TEST_STEP(build_argv_splits_on_whitespace)
{
    dmsystem_unit_t unit = {0};
    int argc = dmsystem_unit_build_argv(&unit, "dmhttpd", "--port 8080  --root /www");

    DMOD_TEST_EXPECT_EQ(argc, 5);
    DMOD_TEST_EXPECT_EQ(unit.argc, 5);
    DMOD_TEST_EXPECT(unit.argv[0] == unit.exec);
    DMOD_TEST_EXPECT(strcmp(unit.argv[0], "dmhttpd") == 0);
    DMOD_TEST_EXPECT(strcmp(unit.argv[1], "--port") == 0);
    DMOD_TEST_EXPECT(strcmp(unit.argv[2], "8080") == 0);
    DMOD_TEST_EXPECT(strcmp(unit.argv[3], "--root") == 0);
    DMOD_TEST_EXPECT(strcmp(unit.argv[4], "/www") == 0);
    DMOD_TEST_EXPECT_NULL(unit.argv[5]);
}

DMOD_TEST_STEP(build_argv_with_no_args)
{
    dmsystem_unit_t unit = {0};
    int argc = dmsystem_unit_build_argv(&unit, "dmnetd", "");

    DMOD_TEST_EXPECT_EQ(argc, 1);
    DMOD_TEST_EXPECT(strcmp(unit.argv[0], "dmnetd") == 0);
    DMOD_TEST_EXPECT_NULL(unit.argv[1]);
}

DMOD_TEST_STEP(parse_dependency_list_mixed_separators)
{
    char deps[DMSYSTEM_MAX_DEPS_PER_UNIT][DMOD_MAX_MODULE_NAME_LENGTH];
    size_t count = dmsystem_unit_parse_dependency_list("networking, storage;;logger  ", deps, DMSYSTEM_MAX_DEPS_PER_UNIT);

    DMOD_TEST_EXPECT_EQ(count, 3u);
    DMOD_TEST_EXPECT(strcmp(deps[0], "networking") == 0);
    DMOD_TEST_EXPECT(strcmp(deps[1], "storage") == 0);
    DMOD_TEST_EXPECT(strcmp(deps[2], "logger") == 0);
}

DMOD_TEST_STEP(parse_dependency_list_empty)
{
    char deps[DMSYSTEM_MAX_DEPS_PER_UNIT][DMOD_MAX_MODULE_NAME_LENGTH];
    DMOD_TEST_EXPECT_EQ(dmsystem_unit_parse_dependency_list("", deps, DMSYSTEM_MAX_DEPS_PER_UNIT), 0u);
    DMOD_TEST_EXPECT_EQ(dmsystem_unit_parse_dependency_list(NULL, deps, DMSYSTEM_MAX_DEPS_PER_UNIT), 0u);
}

DMOD_TEST_STEP(parse_unit_type_and_restart_policy)
{
    DMOD_TEST_EXPECT_EQ((int)dmsystem_unit_parse_type("oneshot"), (int)DMSYSTEM_UNIT_TYPE_ONESHOT);
    DMOD_TEST_EXPECT_EQ((int)dmsystem_unit_parse_type("simple"), (int)DMSYSTEM_UNIT_TYPE_SIMPLE);
    DMOD_TEST_EXPECT_EQ((int)dmsystem_unit_parse_type(NULL), (int)DMSYSTEM_UNIT_TYPE_SIMPLE);
    DMOD_TEST_EXPECT_EQ((int)dmsystem_unit_parse_type("garbage"), (int)DMSYSTEM_UNIT_TYPE_SIMPLE);

    DMOD_TEST_EXPECT_EQ((int)dmsystem_unit_parse_restart("always"), (int)DMSYSTEM_RESTART_ALWAYS);
    DMOD_TEST_EXPECT_EQ((int)dmsystem_unit_parse_restart("no"), (int)DMSYSTEM_RESTART_NO);
    DMOD_TEST_EXPECT_EQ((int)dmsystem_unit_parse_restart(NULL), (int)DMSYSTEM_RESTART_NO);
}

// ===============================================================
//                  Dependency-tree ordering
// ===============================================================

/**
 * @brief Returns the position of the unit named @p name within @p order, or -1
 */
static int position_of(dmsystem_unit_list_t* list, const size_t order[], size_t order_count, const char* name)
{
    dmsystem_unit_t* unit = dmsystem_unit_list_find(list, name);
    if (!unit)
        return -1;

    size_t index = (size_t)(unit - list->units);
    for (size_t i = 0; i < order_count; i++)
    {
        if (order[i] == index)
            return (int)i;
    }

    return -1;
}

DMOD_TEST_STEP(topo_sort_linear_chain)
{
    dmsystem_unit_list_t list;
    dmsystem_unit_list_init(&list);

    dmsystem_unit_list_add(&list, "networking");
    dmsystem_unit_t* webserver = dmsystem_unit_list_add(&list, "webserver");
    webserver->after_count = dmsystem_unit_parse_dependency_list("networking", webserver->after, DMSYSTEM_MAX_DEPS_PER_UNIT);
    dmsystem_unit_t* logger = dmsystem_unit_list_add(&list, "logger");
    logger->after_count = dmsystem_unit_parse_dependency_list("webserver", logger->after, DMSYSTEM_MAX_DEPS_PER_UNIT);

    size_t order[DMSYSTEM_MAX_UNITS];
    size_t order_count = dmsystem_unit_topo_sort(&list, order, DMSYSTEM_MAX_UNITS);

    DMOD_TEST_EXPECT_EQ(order_count, 3u);
    DMOD_TEST_EXPECT(position_of(&list, order, order_count, "networking") <
                      position_of(&list, order, order_count, "webserver"));
    DMOD_TEST_EXPECT(position_of(&list, order, order_count, "webserver") <
                      position_of(&list, order, order_count, "logger"));
}

DMOD_TEST_STEP(topo_sort_diamond_with_requires)
{
    dmsystem_unit_list_t list;
    dmsystem_unit_list_init(&list);

    dmsystem_unit_list_add(&list, "a");

    dmsystem_unit_t* b = dmsystem_unit_list_add(&list, "b");
    b->after_count = dmsystem_unit_parse_dependency_list("a", b->after, DMSYSTEM_MAX_DEPS_PER_UNIT);

    dmsystem_unit_t* c = dmsystem_unit_list_add(&list, "c");
    c->requires_count = dmsystem_unit_parse_dependency_list("a", c->requires, DMSYSTEM_MAX_DEPS_PER_UNIT);

    dmsystem_unit_t* d = dmsystem_unit_list_add(&list, "d");
    d->requires_count = dmsystem_unit_parse_dependency_list("b;c", d->requires, DMSYSTEM_MAX_DEPS_PER_UNIT);

    size_t order[DMSYSTEM_MAX_UNITS];
    size_t order_count = dmsystem_unit_topo_sort(&list, order, DMSYSTEM_MAX_UNITS);

    DMOD_TEST_EXPECT_EQ(order_count, 4u);
    int pos_a = position_of(&list, order, order_count, "a");
    int pos_b = position_of(&list, order, order_count, "b");
    int pos_c = position_of(&list, order, order_count, "c");
    int pos_d = position_of(&list, order, order_count, "d");

    DMOD_TEST_EXPECT(pos_a < pos_b);
    DMOD_TEST_EXPECT(pos_a < pos_c);
    DMOD_TEST_EXPECT(pos_b < pos_d);
    DMOD_TEST_EXPECT(pos_c < pos_d);
}

DMOD_TEST_STEP(topo_sort_unknown_dependency_is_ignored)
{
    dmsystem_unit_list_t list;
    dmsystem_unit_list_init(&list);

    dmsystem_unit_t* unit = dmsystem_unit_list_add(&list, "webserver");
    unit->after_count = dmsystem_unit_parse_dependency_list("does-not-exist", unit->after, DMSYSTEM_MAX_DEPS_PER_UNIT);

    size_t order[DMSYSTEM_MAX_UNITS];
    size_t order_count = dmsystem_unit_topo_sort(&list, order, DMSYSTEM_MAX_UNITS);

    DMOD_TEST_EXPECT_EQ(order_count, 1u);
}

DMOD_TEST_STEP(topo_sort_detects_cycle)
{
    dmsystem_unit_list_t list;
    dmsystem_unit_list_init(&list);

    dmsystem_unit_t* a = dmsystem_unit_list_add(&list, "a");
    a->after_count = dmsystem_unit_parse_dependency_list("b", a->after, DMSYSTEM_MAX_DEPS_PER_UNIT);

    dmsystem_unit_t* b = dmsystem_unit_list_add(&list, "b");
    b->after_count = dmsystem_unit_parse_dependency_list("a", b->after, DMSYSTEM_MAX_DEPS_PER_UNIT);

    size_t order[DMSYSTEM_MAX_UNITS];
    size_t order_count = dmsystem_unit_topo_sort(&list, order, DMSYSTEM_MAX_UNITS);

    DMOD_TEST_EXPECT_EQ(order_count, 0u);
}
