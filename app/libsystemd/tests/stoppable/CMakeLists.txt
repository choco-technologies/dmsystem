# test_stoppable - helper Application spawned by test_libsystemd's stop steps
# (fixtures/stop/*.ini). Never released.
set(DMOD_MODULE_NAME        test_stoppable)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         1024)
set(DMOD_PRIORITY           0)

dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    stoppable.c
)

dmod_link_modules(${DMOD_MODULE_NAME}
    dmosi
    dmlist
)

target_link_libraries(${DMOD_MODULE_NAME}
    libsystemd_if
    )
