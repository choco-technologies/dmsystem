# test_libsystemd_early - the front (libsystemd) with no implementation behind it: what it answers, what it keeps and what it
# gives libsystemd_core when that is loaded later (the order of the boot). It requires nothing but libsystemd, on purpose:
# libsystemd_core is loaded by the test itself.
set(DMOD_MODULE_NAME        test_libsystemd_early)
set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         1024)
set(DMOD_PRIORITY           0)

dmod_add_test(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    early_test.c
)

dmod_link_modules(${DMOD_MODULE_NAME}
    dmosi
    dmfsi
    )

target_link_libraries(${DMOD_MODULE_NAME}
    libsystemd_if
    )

target_compile_definitions(${DMOD_MODULE_NAME}
    PRIVATE
        LIBSYSTEMD_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures"
    )
