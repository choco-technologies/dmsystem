# Name of the module
set(DMOD_MODULE_NAME        service)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         2048)

# Priority for the thread of the module (should be integer)
set(DMOD_PRIORITY           0)

# Path to the .dmr file - enables release package creation in build/packages/
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/service.dmr)

# List of MAL interfaces implemented by this module (leave empty if none)
set(DMOD_MAL_IMPLS          )

#
#   dmod_add_executable - create an executable module
#   it has the same signature as add_executable,
#   and can be used in the same way after the creation
#   (for example, to link libraries)
#
# `service` is a small systemctl/service-alike CLI for inspecting and
# controlling the units a running `systemd` is managing. It talks to the
# exact same in-memory unit registry `systemd` uses, by calling into the
# shared `libsystemd` Library module - see README.md for the caveat that this
# only sees a populated registry once some `systemd`/`libsystemd_scan()` call
# has actually run in this process.
#
dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/service.c
)

# libsystemd.h and libsystemd_types.h transitively depend on dmlist's and
# dmosi's types.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmlist
    dmosi
)

# libsystemd is a local sibling module built in this same repository, so its
# headers come straight from its own `_if` interface target rather than a
# dmod_link_modules() fetch (which is for genuinely external, separately
# published modules).
target_link_libraries(${DMOD_MODULE_NAME}
    libsystemd_if
    )

add_subdirectory(tests)
