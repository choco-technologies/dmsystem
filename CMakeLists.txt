cmake_minimum_required(VERSION 3.18)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED DMOD_MODULE_VERSION)
    set(DMOD_MODULE_VERSION "0.1" CACHE STRING "DMOD module version")
endif()

# If a local dmod checkout is provided, use it instead of fetching from git
if(DEFINED DMOD_DIR AND NOT "${DMOD_DIR}" STREQUAL "")
    set(FETCHCONTENT_SOURCE_DIR_DMOD "${DMOD_DIR}" CACHE PATH "" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(
    dmod
    GIT_REPOSITORY https://github.com/choco-technologies/dmod.git
    GIT_TAG        develop
)

set(DMOD_MODE "DMOD_MODULE" CACHE STRING "DMOD build mode")
set(DMOD_BUILD_TESTS OFF CACHE BOOL "Build tests")
set(DMOD_BUILD_EXAMPLES OFF CACHE BOOL "Build examples")
set(DMOD_BUILD_TOOLS OFF CACHE BOOL "Build tools")
set(DMOD_BUILD_TEMPLATES OFF CACHE BOOL "Build templates")

FetchContent_MakeAvailable(dmod)

project(dmsystem VERSION ${DMOD_MODULE_VERSION}
    DESCRIPTION "dmsystem DMOD application module"
    LANGUAGES C CXX)

set(DMOD_DIR ${dmod_SOURCE_DIR} CACHE PATH "DMOD source directory")
set(DMOD_SCRIPTS_DIR ${DMOD_DIR}/scripts CACHE PATH "DMOD scripts directory")
include(${DMOD_DIR}/paths.cmake)
dmod_setup_external_module()

# Name of the module
set(DMOD_MODULE_NAME        dmsystem)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "John Doe")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         4096)

# Priority for the thread of the module (should be integer)
set(DMOD_PRIORITY           0)

# Path to the .dmr file - enables release package creation in build/packages/
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/dmsystem.dmr)

# List of MAL interfaces implemented by this module (leave empty if none)
set(DMOD_MAL_IMPLS          )

# dmsystem's own orchestration logic lives in two separate DMOD library modules
# rather than being baked directly into this executable, so it is real, testable
# DMOD module API:
#   - dmsystem_unit:  unit model + dependency-graph ordering (no dmini/dmosi dependency)
#   - dmsystem_core:  INI loading (dmini) + process supervision (dmosi) + orchestration,
#                     built on top of dmsystem_unit
# See lib/dmsystem_unit/CMakeLists.txt and lib/dmsystem_core/CMakeLists.txt.
if(DMOD_MODE STREQUAL "DMOD_MODULE")
    add_subdirectory(lib/dmsystem_unit)
    add_subdirectory(lib/dmsystem_core)
endif()

#
#   dmod_add_executable - create an executable module
#   it has the same signature as add_executable,
#   and can be used in the same way after the creation
#   (for example, to link libraries)
#
dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    # List of source files - can include C and C++ files
    src/dmsystem.c
)

# dmsystem only ever calls dmsystem_core's single dmsystem_core_run() entry point -
# everything else is dmsystem_core's own concern (see the comment above).
target_link_libraries(${DMOD_MODULE_NAME} dmsystem_core_if)

enable_testing()
add_subdirectory(tests)
