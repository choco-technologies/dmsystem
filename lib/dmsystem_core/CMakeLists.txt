# =====================================================================
#               dmsystem_core Library Module
# =====================================================================
# The runtime orchestrator: loads the INI config (via dmini), asks dmsystem_unit
# for the dependency order, and starts/supervises units (via dmosi). The pure
# unit-model/graph logic lives in the separate dmsystem_unit module instead of
# here specifically so it can be tested without a live dmini/dmosi backend - see
# lib/dmsystem_unit/CMakeLists.txt.
set(DMOD_MODULE_NAME        dmsystem_core)
set(DMOD_MODULE_VERSION     "0.1")
set(DMOD_AUTHOR_NAME        "John Doe")
set(DMOD_STACK_SIZE         4096)

# Path to the .dmr file - enables release package creation in build/packages/.
# dmsystem_core is required by dmsystem at runtime (see src/dmsystem.c), so it must be
# packaged and released alongside it, not just built as a local implementation detail.
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/dmsystem_core.dmr)

dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    dmsystem_config.c
    dmsystem_proc.c
    dmsystem_core.c
)

# dmsystem_unit is a local module (built in this same repo) - consumed the same way
# dmell links dmell_core_if, via the auto-generated _if INTERFACE target, not via
# dmod_link_modules (that's reserved for genuinely external, separately-released
# modules such as dmini/dmosi below). This link is private to dmsystem_core's own
# sources (they need the full dmsystem_unit.h/dmsystem_graph.h API + generated defs).
target_link_libraries(${DMOD_MODULE_NAME} dmsystem_unit_if)

# dmsystem_core.h (a public header, exposed via dmsystem_core_if) includes
# dmsystem_unit_types.h for dmsystem_unit_status_t - unlike the internal link above,
# this needs to reach dmsystem_core's own consumers (service, dmsystem, test_dmsystem)
# too, so it is added directly to dmsystem_core_if's own INTERFACE include path rather
# than relying on transitive propagation (target_link_libraries above uses the plain,
# private-by-default signature already used elsewhere for this target, and mixing it
# with a PUBLIC-keyword call on the same target is not allowed).
target_include_directories(${DMOD_MODULE_NAME}_if INTERFACE
    ${CMAKE_SOURCE_DIR}/lib/dmsystem_unit
)

# dmini (config parsing) and dmosi (process supervision) are external DMOD
# modules - link their headers via dmf-get instead of vendoring their source,
# same as any other external module dependency (see dmell/lib/dmell_core).
dmod_link_modules(${DMOD_MODULE_NAME}
    dmini
    dmosi
)
