# ReXNative: adds the native D3D12 renderer (with DLSS / DLAA / DLSS 5, FSR and the settings HUD)
# to a ReXGlue v0.11 game.
#
#   include(<ReXNative>/renderer/rexnative.cmake)
#   rexnative_add(<target>              # the game's host library (rexglue_setup_host_library) or
#                                       # its executable
#       LAYER_DIR       <dir>   # this game's game_layer.h and d3d_surface.inc (rexnative adapt)
#       REXGLUE_SDK_SRC <dir>   # the source tree the ReXGlue SDK was built from
#       FIDELITYFX_SDK  <dir>   # AMD FidelityFX SDK v2.4.0
#       DLSS_SDK        <dir>   # NVIDIA DLSS SDK (include/, lib/Windows_x86_64/x64); Windows
#       DLSS_NR         <dir>   # NVIDIA DLSS 5 (neural rendering) headers; Windows
#       DXVK_NATIVE_INCLUDE <dirs> # Linux: DXVK Native's installed include/dxvk (its Windows
#                               # and DirectX headers), after a folder with a newer d3d12.h
#                               # (scripts/linux/build-vkd3d-dxvk.sh: mingw-w64's)
#       [EXECUTABLE <exe>]      # where the runtime DLLs go (default: <target>)
#       [NO_DEFAULT_GLUE])      # the game defines the glue functions (src/glue_defaults.cpp)
#
# D3D12. On Windows (and under Proton on the Steam Deck) the system's; on Linux (experimental)
# vkd3d-proton's native libvkd3d-proton-d3d12.so with DXVK Native's DXGI, libdxvk_dxgi.so, both
# loaded at run time (scripts/linux/build-vkd3d-dxvk.sh builds them; src/linux and
# src/compat/linux stand in for the Win32 calls and DLSS there, and WineHQ vkd3d's
# libvkd3d-utils.so.1 for d3dcompiler_47's D3DCompile). The game's app
# connects the renderer to the SDK (src/host.h): rexnative::host::Attach at OnInit.
set(REXNATIVE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(rexnative_add target)
    cmake_parse_arguments(RN "NO_DEFAULT_GLUE"
        "LAYER_DIR;REXGLUE_SDK_SRC;FIDELITYFX_SDK;DLSS_SDK;DLSS_NR;EXECUTABLE"
        "DXVK_NATIVE_INCLUDE" ${ARGN})
    if(WIN32)
        set(required LAYER_DIR REXGLUE_SDK_SRC FIDELITYFX_SDK DLSS_SDK DLSS_NR)
    else()
        set(required LAYER_DIR REXGLUE_SDK_SRC FIDELITYFX_SDK DXVK_NATIVE_INCLUDE)
    endif()
    foreach(arg ${required})
        if(NOT RN_${arg})
            message(FATAL_ERROR "rexnative_add(${target}): ${arg} is required")
        endif()
    endforeach()
    if(NOT RN_EXECUTABLE)
        set(RN_EXECUTABLE ${target})
    endif()
    if(NOT EXISTS "${RN_LAYER_DIR}/game_layer.h")
        message(FATAL_ERROR "rexnative_add(${target}): no game_layer.h in ${RN_LAYER_DIR} "
                            "(python -m rexnative adapt writes it)")
    endif()
    set(src "${REXNATIVE_DIR}/src")
    file(GLOB native_sources CONFIGURE_DEPENDS "${src}/native/*.cpp")
    if(NOT WIN32)
        # Linux: no NGX (DLSS), no Win32 debug registers (watch), no PGO counter file writer.
        list(REMOVE_ITEM native_sources "${src}/native/dlss.cpp" "${src}/native/watch.cpp"
            "${src}/native/pgo_counters.cpp")
        list(APPEND native_sources "${src}/linux/win32.cpp" "${src}/linux/d3d12_loader.cpp"
            "${src}/linux/d3dcompile_vkd3d.cpp" "${src}/linux/dlss_none.cpp"
            "${src}/linux/watch_none.cpp" "${src}/linux/profiler.cpp")
        # Every renderer source gets the Win32 stand-ins first.
        set_source_files_properties(${native_sources} "${src}/rexnative_d3d_hooks.cpp"
            "${src}/hud/hud.cpp" "${src}/host.cpp" "${src}/glue_defaults.cpp" PROPERTIES
            COMPILE_OPTIONS "-include;${src}/compat/linux/rexnative_win32.h")
    endif()
    target_sources(${target} PRIVATE
        "${src}/rexnative_d3d_hooks.cpp"
        "${src}/hud/hud.cpp"
        "${src}/host.cpp"
        ${native_sources})
    if(NOT RN_NO_DEFAULT_GLUE)
        target_sources(${target} PRIVATE "${src}/glue_defaults.cpp")
    endif()
    # Before the game's own include directories: game_layer.h and d3d_surface.inc come from the
    # layer and native/... from here, even where a port still has older copies of either.
    target_include_directories(${target} BEFORE PRIVATE "${RN_LAYER_DIR}" "${src}"
        "${src}/native/shaders" "${src}/compat")
    if(NOT RN_EXECUTABLE STREQUAL target)
        # The app's own sources include host.h and hud/hud.h.
        target_include_directories(${RN_EXECUTABLE} PRIVATE "${src}" "${src}/compat")
    endif()
    if(NOT WIN32)
        target_include_directories(${target} BEFORE PRIVATE "${src}/compat/linux")
        target_include_directories(${target} SYSTEM PRIVATE "${RN_DXVK_NATIVE_INCLUDE}")
        target_compile_definitions(${target} PRIVATE REXNATIVE_LINUX WIDL_EXPLICIT_AGGREGATE_RETURNS
            NOMINMAX)
        # FidelityFX's headers mark their exports with __declspec.
        target_compile_options(${target} PRIVATE -fdeclspec)
    endif()
    target_include_directories(${target} SYSTEM PRIVATE
        "${RN_FIDELITYFX_SDK}/Kits/FidelityFX/api/include"
        "${RN_FIDELITYFX_SDK}/Kits/FidelityFX/upscalers/include"
        "${RN_FIDELITYFX_SDK}/Kits/FidelityFX/framegeneration/include"
        "${RN_DLSS_SDK}/include" "${RN_DLSS_NR}/include")
    target_compile_options(${target} PRIVATE -Wno-unused-parameter -Wno-missing-field-initializers
        -Wno-deprecated-declarations)
    if(NOT WIN32)
        # The game window's size (linux/win32.cpp GetClientRect): the SDK's SDL3.
        find_package(SDL3 REQUIRED CONFIG COMPONENTS SDL3-shared)
        target_link_libraries(${target} PRIVATE ${CMAKE_DL_LIBS} SDL3::SDL3-shared)
        if(NOT TARGET rexnative_shader_xlate)
            set(REXNATIVE_REXGLUE_SDK_SRC "${RN_REXGLUE_SDK_SRC}")
            include("${src}/native/shader_xlate.cmake")
        endif()
        target_link_libraries(${target} PRIVATE rexnative_shader_xlate)
        return()
    endif()
    target_link_libraries(${target} PRIVATE d3d12 dxgi dxguid delayimp
        "${RN_DLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
    target_link_options(${RN_EXECUTABLE} PRIVATE "LINKER:/DELAYLOAD:d3d12.dll")
    # The runtime DLLs the renderer loads: NVIDIA's signed DLSS and DLSS 5 (neural rendering),
    # AMD's signed FidelityFX loader / upscaler / frame generation (413 MB). Without them DLSS / FSR
    # report "not available" and the scene keeps its MSAA. With REXNATIVE_COMMON_DIR (python -m
    # rexnative project: <work>/common) they go there once for every game and a copy an older build
    # left beside the executable goes (native/common_dir.h); else beside the executable.
    foreach(dll
            "${RN_DLSS_SDK}/lib/Windows_x86_64/rel/nvngx_dlss.dll"
            "${RN_DLSS_NR}/lib/Windows_x86_64/rel/nvngx_dlssnr.dll"
            "${RN_FIDELITYFX_SDK}/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll"
            "${RN_FIDELITYFX_SDK}/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll"
            "${RN_FIDELITYFX_SDK}/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_dx12.dll")
        if(EXISTS "${dll}" AND REXNATIVE_COMMON_DIR)
            get_filename_component(dll_name "${dll}" NAME)
            add_custom_command(TARGET ${RN_EXECUTABLE} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${REXNATIVE_COMMON_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "${REXNATIVE_COMMON_DIR}"
                COMMAND ${CMAKE_COMMAND} -E rm -f "$<TARGET_FILE_DIR:${RN_EXECUTABLE}>/${dll_name}"
                VERBATIM)
        elseif(EXISTS "${dll}")
            add_custom_command(TARGET ${RN_EXECUTABLE} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}"
                        "$<TARGET_FILE_DIR:${RN_EXECUTABLE}>"
                VERBATIM)
        else()
            message(WARNING "rexnative_add(${target}): ${dll} not found; that feature stays off")
        endif()
    endforeach()
    if(NOT TARGET rexnative_shader_xlate)
        set(REXNATIVE_REXGLUE_SDK_SRC "${RN_REXGLUE_SDK_SRC}")
        include("${src}/native/shader_xlate.cmake")
    endif()
    target_link_libraries(${target} PRIVATE rexnative_shader_xlate)
endfunction()
