# =====================================================================
#               dmsystem_unit Library Module
# =====================================================================
# The unit model and dependency-graph ordering - pure logic with no dependency on
# dmini or dmosi, kept in its own module (separate from dmsystem_core) specifically
# so that test_dmsystem can load it standalone: dmod_loader refuses to enable a
# module unless *every* module it requires can also be loaded, so if this logic
# lived inside dmsystem_core (which requires dmini/dmosi), testing it would need a
# live dmini/dmosi backend even though these particular functions never call them.
set(DMOD_MODULE_NAME        dmsystem_unit)
set(DMOD_MODULE_VERSION     "0.1")
set(DMOD_AUTHOR_NAME        "John Doe")
set(DMOD_STACK_SIZE         4096)

# Path to the .dmr file - enables release package creation in build/packages/.
# dmsystem_unit is required by dmsystem_core at runtime, so it must be packaged
# and released alongside it, not just built as a local implementation detail.
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/dmsystem_unit.dmr)

dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    dmsystem_unit.c
    dmsystem_graph.c
)
