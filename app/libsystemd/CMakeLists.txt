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
#
# libsystemd is the front of the libsystemd API: the whole API (libsystemd.h) and the reports that come before the
# implementation is there, in a few hundred bytes of code. The implementation (units, rules, supervision) is the module
# libsystemd_core (../libsystemd_core), which is loaded with the first service manager, implements the dif of libsystemd.h and calls
# libsystemd_deliver_reports(). Modules that only report devices (dmdevfs, dmtty, ...) need nothing but this one - and it is the one
# that has to be early in the boot, in the memory that is there then.
#
dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/libsystemd.c
)

target_include_directories(${DMOD_MODULE_NAME}_if
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        )

# dmfsi: the _mount_changed dif libsystemd implements (reports mounts as device events) - a header only
dmod_link_modules(${DMOD_MODULE_NAME}
    dmlist
    dmosi
    dmfsi
)

add_subdirectory(tests)
