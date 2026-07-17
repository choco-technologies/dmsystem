# =====================================================================
#               dmsystem unit tests (dmod_test.h framework)
# =====================================================================
# dmsystem_test.c calls dmsystem_unit's unit/graph functions as real DMOD module
# API - it links only against dmsystem_unit_if (headers), never dmsystem_unit's
# source directly. At run time, dmod_loader resolves dmsystem_unit.dmf the same
# way it resolves any other required module, finding it in DMOD_DMF_DIR since
# both test_dmsystem and dmsystem_unit are built into the same project's dmf
# output directory. dmsystem_unit (unlike dmsystem_core) has no dmini/dmosi
# dependency, so this never needs a live backend for either of those connected
# just to load it.
set(DMOD_MODULE_NAME        test_dmsystem)
set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "John Doe")
# Several test steps declare a dmsystem_unit_list_t as a plain stack local (unlike
# dmsystem_core_run, which heap-allocates it) - at DMSYSTEM_MAX_UNITS units, each
# carrying argv storage, dependency name arrays and stream-redirection paths, that
# struct is tens of KB, well beyond a typical embedded module's default stack.
set(DMOD_STACK_SIZE         131072)
set(DMOD_PRIORITY           0)

dmod_add_test(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    dmsystem_test.c
)

target_link_libraries(${DMOD_MODULE_NAME} dmsystem_unit_if)

find_program(DMOD_LOADER dmod_loader)
if(DMOD_LOADER)
    add_test(NAME ${DMOD_MODULE_NAME} COMMAND ${DMOD_LOADER} ${DMOD_DMF_DIR}/${DMOD_MODULE_NAME}.dmf)
    set_tests_properties(${DMOD_MODULE_NAME} PROPERTIES ENVIRONMENT "DMOD_DMF_DIR=${DMOD_DMF_DIR}")
else()
    message(WARNING "dmod_loader not found - tests will build but ctest will not be able to run them")
endif()
