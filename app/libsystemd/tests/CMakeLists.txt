set(DMOD_MODULE_NAME        test_libsystemd)
set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         1024)
set(DMOD_PRIORITY           0)

dmod_add_test(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    libsystemd_test.c
)

dmod_link_modules(${DMOD_MODULE_NAME}
    dmosi
    dmlist
    )

target_link_libraries(${DMOD_MODULE_NAME}
    libsystemd_if
    )

# Absolute paths to the example unit files (shipped, user-facing) and to the
# test-only fixtures (invalid ini files etc.), so the tests do not depend on
# the process's current working directory.
target_compile_definitions(${DMOD_MODULE_NAME}
    PRIVATE
        LIBSYSTEMD_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../examples"
        LIBSYSTEMD_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    )
