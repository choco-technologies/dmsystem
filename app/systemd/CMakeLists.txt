# Name of the module
set(DMOD_MODULE_NAME        systemd)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         2048)

# Priority for the thread of the module (should be integer)
#
# The highest priority a module can ask for. systemd is what brings the rest of
# the system up - it scans the units directory, starts every unit and then
# replays the devices drivers reported while it was still booting - so nothing
# a unit does afterwards should be able to push it aside. It was previously 0,
# i.e. below the default of 1 that every module without an explicit priority
# gets: a shell started by one of systemd's own units outranked it, and any
# thread of that shell that failed to block would stop the boot pass halfway
# through (see DMOD_PRIORITY's default in dmod/scripts/CMakeLists.txt).
#
# Values above the backend's ceiling are clamped by the RTOS (FreeRTOS clamps
# to configMAX_PRIORITIES - 1), so this stays valid on a port with a smaller
# priority range.
set(DMOD_PRIORITY           4)

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
# `systemd` itself is intentionally thin: argument parsing and a one-shot
# scan-and-start pass, nothing else. All of the actual unit-file parsing,
# dependency-ordering and process supervision logic lives in `libsystemd`
# (a Library module), so it can also be enabled as a dependency of `service`
# and of its own tests - only Library modules can be enabled as a required
# dependency of another module (see Dmod_Enable in the dmod core).
#
dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/systemd.c
)

# libsystemd.h and libsystemd_types.h transitively depend on dmlist's and
# dmosi's types, so their headers still need to be fetched even though this
# module never calls dmlist's/dmosi's API directly.
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
