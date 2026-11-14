# ReXNative: adds the native D3D12 renderer (with DLSS / DLAA / DLSS 5 and the settings HUD) to a
# ReXGlue game target. From Iron Brigade's and From Dust's CMakeLists.
#
#   include(<ReXNative>/renderer/rexnative.cmake)
#   rexnative_add(<target>
#       LAYER_DIR       <dir>   # this game's game_layer.h and d3d_surface.inc (rexnative adapt)
#       REXGLUE_SDK_SRC <dir>   # the source tree the ReXGlue SDK was built from
#       FIDELITYFX_SDK  <dir>   # AMD FidelityFX SDK v2.3.0
#       DLSS_SDK        <dir>   # NVIDIA DLSS SDK (include/, lib/Windows_x86_64/x64)
#       DLSS_NR         <dir>   # NVIDIA DLSS 5 (neural rendering) headers
#       [NO_DEFAULT_GLUE])      # the game defines the glue functions (src/glue_defaults.cpp)
#
# Windows only (D3D12; the Steam Deck runs the Windows build under Proton). The game's app also
# calls the HUD (src/hud/hud.h): SetConfigPath, Initialize, Shutdown.
set(REXNATIVE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(rexnative_add target)
    cmake_parse_arguments(RN "NO_DEFAULT_GLUE" "LAYER_DIR;REXGLUE_SDK_SRC;FIDELITYFX_SDK;DLSS_SDK;DLSS_NR" "" ${ARGN})
    foreach(arg LAYER_DIR REXGLUE_SDK_SRC FIDELITYFX_SDK DLSS_SDK DLSS_NR)
        if(NOT RN_${arg})
            message(FATAL_ERROR "rexnative_add(${target}): ${arg} is required")
        endif()
    endforeach()
    if(NOT EXISTS "${RN_LAYER_DIR}/game_layer.h")
        message(FATAL_ERROR "rexnative_add(${target}): no game_layer.h in ${RN_LAYER_DIR} "
                            "(python -m rexnative adapt writes it)")
    endif()
    set(src "${REXNATIVE_DIR}/src")
    file(GLOB native_sources CONFIGURE_DEPENDS "${src}/native/*.cpp")
    target_sources(${target} PRIVATE
        "${src}/rexnative_d3d_hooks.cpp"
        "${src}/hud/hud.cpp"
        ${native_sources}
        "${RN_REXGLUE_SDK_SRC}/src/graphics/pipeline/texture/util.cpp"
        "${RN_REXGLUE_SDK_SRC}/src/graphics/pipeline/texture/info_formats.cpp")
    if(NOT RN_NO_DEFAULT_GLUE)
        target_sources(${target} PRIVATE "${src}/glue_defaults.cpp")
    endif()
    # Before the game's own include directories: game_layer.h and d3d_surface.inc come from the
    # layer and native/... from here, even where a port still has older copies of either.
    target_include_directories(${target} BEFORE PRIVATE "${RN_LAYER_DIR}" "${src}"
        "${RN_REXGLUE_SDK_SRC}/src/ui/shaders/bytecode")
    target_include_directories(${target} SYSTEM PRIVATE
        "${RN_FIDELITYFX_SDK}/Kits/FidelityFX/api/include"
        "${RN_FIDELITYFX_SDK}/Kits/FidelityFX/upscalers/include"
        "${RN_FIDELITYFX_SDK}/Kits/FidelityFX/framegeneration/include"
        "${RN_DLSS_SDK}/include" "${RN_DLSS_NR}/include")
    target_link_libraries(${target} PRIVATE d3d12 dxgi dxguid delayimp
        "${RN_DLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
    target_link_options(${target} PRIVATE /DELAYLOAD:d3d12.dll)
    if(NOT TARGET rexnative_shader_xlate)
        set(REXNATIVE_REXGLUE_SDK_SRC "${RN_REXGLUE_SDK_SRC}")
        include("${src}/native/shader_xlate.cmake")
    endif()
    target_link_libraries(${target} PRIVATE rexnative_shader_xlate)
endfunction()
