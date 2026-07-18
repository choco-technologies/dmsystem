# Name of the module
set(DMOD_MODULE_NAME        systemd)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         2048)

# Priority for the thread of the module (should be integer)
set(DMOD_PRIORITY           0)

# Path to the .dmr file - enables release package creation in build/packages/
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/systemd.dmr)

# List of MAL interfaces implemented by this module (leave empty if none)
set(DMOD_MAL_IMPLS          )

#
#   dmod_add_executable - create an executable module
#   it has the same signature as add_executable,
#   and can be used in the same way after the creation
#   (for example, to link libraries)
#
dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    # List of source files - can include C and C++ files
    src/systemd.c
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