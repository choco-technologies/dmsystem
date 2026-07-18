# Name of the module
set(DMOD_MODULE_NAME        libsystemd)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         2048)

# Priority for the thread of the module (should be integer)
set(DMOD_PRIORITY           0)

# Path to the .dmr file - enables release package creation in build/packages/
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/libsystemd.dmr)

# List of MAL interfaces implemented by this module (leave empty if none)
set(DMOD_MAL_IMPLS          )

#
#   dmod_add_library - create a library module
#   it has the same signature as add_library
#   and can be used in the same way after the creation
#   (for example, to link libraries)
#
# This is a Library (not an Application): it holds the shared unit registry
# and scan/start/stop/status/list logic, so that it can be enabled as a
# required module by multiple Application front-ends (systemd, service) and
# by its own tests - only Library modules can be enabled as a dependency of
# another module (see Dmod_Enable in the dmod core).
#
dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    # List of source files - can include C and C++ files
    src/serviceapi.c
)

target_include_directories(${DMOD_MODULE_NAME}_if
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        )

# To depend on another module's headers, use:
dmod_link_modules(${DMOD_MODULE_NAME}
    dmlist
    dmini
    dmosi
)

add_subdirectory(tests)
