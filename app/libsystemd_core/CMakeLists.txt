# Name of the module
set(DMOD_MODULE_NAME        libsystemd_core)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         2048)

# Priority for the thread of the module (should be integer)
set(DMOD_PRIORITY           0)

# Path to the .dmr file - enables release package creation in build/packages/
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/libsystemd_core.dmr)

# List of MAL interfaces implemented by this module (leave empty if none)
set(DMOD_MAL_IMPLS          )

#
#   dmod_add_library - create a library module
#
# The implementation of libsystemd: the registry of the units and of the rules, the parsing, the starting order, the
# supervision of the processes, the replay of the devices. It offers no API of its own: it implements the dif of libsystemd.h
# (_impl_*) for the front (module libsystemd, ../libsystemd), calls libsystemd_deliver_reports() when it is loaded, and the front
# passes the calls of libsystemd.h on. That is why it can be loaded later than the front - with the first service manager, when the external RAM
# is there - while the modules that report devices (dmdevfs, dmtty, ...) need only the front, early.
#
# It is a Library (not an Application), so that systemd, service and the tests can require it (see Dmod_Enable in the dmod core).
#
dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/serviceapi.c
    src/stoprequest.c
)


target_include_directories(${DMOD_MODULE_NAME}_if
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        )

dmod_link_modules(${DMOD_MODULE_NAME}
    dmlist
    dmini
    dmosi
    dmfsi
)

# The headers of the front (the types, the dif of the units) and the module itself, which has to be there for
# libsystemd_deliver_reports()
target_link_libraries(${DMOD_MODULE_NAME}
    libsystemd_if
    )
