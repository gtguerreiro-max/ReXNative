# rexnative_shader_xlate: ReXGlue's Xenos shader analysis and DXBC shader translator (BSD-3, from
# Xenia), built from the SDK source tree as a static library for the game. In the SDK these
# sources only exist inside the rexgpu-xenos-d3d12 driver module, which exports nothing.
#
#   set(REXNATIVE_REXGLUE_SDK_SRC <SDK source root>)
#   include(<this file>)
#   target_link_libraries(<target> PRIVATE rexnative_shader_xlate)
#
# Requires the rex::sdk imported target (find_package(rexglue)).
if(NOT REXNATIVE_REXGLUE_SDK_SRC)
    message(FATAL_ERROR "shader_xlate.cmake: set REXNATIVE_REXGLUE_SDK_SRC")
endif()
set(_rexnative_shader "${REXNATIVE_REXGLUE_SDK_SRC}/src/gpu/xenos/pipeline/shader")
add_library(rexnative_shader_xlate STATIC
    ${_rexnative_shader}/shader.cpp
    ${_rexnative_shader}/translator.cpp
    ${_rexnative_shader}/translator_disasm.cpp
    ${_rexnative_shader}/dxbc.cpp
    ${_rexnative_shader}/dxbc_translator.cpp
    ${_rexnative_shader}/dxbc_translator_alu.cpp
    ${_rexnative_shader}/dxbc_translator_fetch.cpp
    ${_rexnative_shader}/dxbc_translator_memexport.cpp
    ${_rexnative_shader}/dxbc_translator_om.cpp
    ${REXNATIVE_REXGLUE_SDK_SRC}/src/gpu/xenos/draw_state.cpp
    ${REXNATIVE_REXGLUE_SDK_SRC}/src/gpu/xenos/host/fixed_point.cpp
    ${REXNATIVE_REXGLUE_SDK_SRC}/thirdparty/dxbc/DXBCChecksum.cpp
    ${CMAKE_CURRENT_LIST_DIR}/xlate/xxhash_impl.cpp
)
target_include_directories(rexnative_shader_xlate PUBLIC
    "${REXNATIVE_REXGLUE_SDK_SRC}/src" "${REXNATIVE_REXGLUE_SDK_SRC}"
    "${REXNATIVE_REXGLUE_SDK_SRC}/thirdparty/xxHash")
# The SDK's own build of these sources: the translator's export macro empty, the SDK's warnings.
target_compile_definitions(rexnative_shader_xlate PUBLIC REX_GPU_XENOS_STATIC)
target_compile_options(rexnative_shader_xlate PRIVATE -msse4.1 -Wno-unused-variable
    -Wno-unused-parameter -Wno-deprecated-declarations)
set_source_files_properties(${REXNATIVE_REXGLUE_SDK_SRC}/thirdparty/dxbc/DXBCChecksum.cpp
    ${CMAKE_CURRENT_LIST_DIR}/xlate/xxhash_impl.cpp PROPERTIES COMPILE_OPTIONS -w)
target_link_libraries(rexnative_shader_xlate PUBLIC rex::sdk)
unset(_rexnative_shader)
