set(DMOD_MODULE_NAME dmtestlibrary)
set(DMOD_DMR_PATH ${CMAKE_CURRENT_SOURCE_DIR}/dmtestlibrary.dmr)
set(DMOD_DIF_IMPLS libsystemd)
dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION} library.c)
target_include_directories(${DMOD_MODULE_NAME}_if INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${DMOD_MODULE_NAME} libsystemd_if)

set(DMOD_MODULE_NAME dmtestcold)
set(DMOD_DMR_PATH ${CMAKE_CURRENT_SOURCE_DIR}/dmtestcold.dmr)
dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION} cold.c)
target_link_libraries(${DMOD_MODULE_NAME} libsystemd_if)

# No lifecycle DIF and no imported API: exercise the legacy load/enable and
# disable/unload path independently of shared consumer dependencies.
set(DMOD_MODULE_NAME dmtestplain)
set(DMOD_DMR_PATH ${CMAKE_CURRENT_SOURCE_DIR}/dmtestplain.dmr)
set(DMOD_DIF_IMPLS "")
dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION} plain.c)
