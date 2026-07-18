set(DMOD_MODULE_NAME        test_service)
set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         1024)
set(DMOD_PRIORITY           0)

dmod_add_test(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    service_test.c
)

dmod_link_modules(${DMOD_MODULE_NAME}
    dmlist
    dmosi
    )

# libsystemd is the local sibling Library that actually holds the API being
# exercised here - see app/service/CMakeLists.txt for why this uses the
# `_if` interface target instead of dmod_link_modules().
target_link_libraries(${DMOD_MODULE_NAME}
    libsystemd_if
    )
