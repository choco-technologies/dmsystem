# =====================================================================
#               service Application Module
# =====================================================================
# A systemctl/`service`-alike CLI: inspects and controls the units a running
# dmsystem is managing, by calling dmsystem_core's query/control API directly
# (see lib/dmsystem_core/dmsystem_core.h) - not by talking to dmsystem itself.
set(DMOD_MODULE_NAME        service)
set(DMOD_MODULE_VERSION     "0.1")
set(DMOD_AUTHOR_NAME        "John Doe")
set(DMOD_STACK_SIZE         2048)
set(DMOD_PRIORITY           0)

# Path to the .dmr file - enables release package creation in build/packages/
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/service.dmr)

dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    service.c
)

# dmsystem_core is a local module (built in this same repo) - consumed via the
# auto-generated _if INTERFACE target, same as dmsystem's own executable.
target_link_libraries(${DMOD_MODULE_NAME} dmsystem_core_if)
