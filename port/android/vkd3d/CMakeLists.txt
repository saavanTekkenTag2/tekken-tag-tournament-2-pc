# vkd3d-shader (HLSL -> DXBC compiler) for Android, built from the copy Wine bundles in
# libs/vkd3d (wine-mirror/wine on GitHub; vkd3d 1.18, LGPL-2.1+). Only the shader library and
# vkd3d-common are built (no libvkd3d / Vulkan); inc/ supplies the few Windows / D3D headers it
# includes. Used by runtime/d3dcompile_vkd3d.cpp, the D3DCompile the renderer calls at run time.
#   VKD3D_SOURCE = <wine checkout>/libs/vkd3d
cmake_minimum_required(VERSION 3.20)
if(NOT EXISTS "${VKD3D_SOURCE}/libs/vkd3d-shader/hlsl.y")
    message(FATAL_ERROR "VKD3D_SOURCE (wine libs/vkd3d) missing: ${VKD3D_SOURCE}")
endif()
find_program(BISON_EXE bison REQUIRED)
find_program(FLEX_EXE flex REQUIRED)
set(GEN "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${GEN}")
set(GENERATED "")
foreach(g hlsl preproc)
    add_custom_command(OUTPUT "${GEN}/${g}.tab.c" "${GEN}/${g}.tab.h"
        COMMAND "${BISON_EXE}" -d -o "${GEN}/${g}.tab.c" "${VKD3D_SOURCE}/libs/vkd3d-shader/${g}.y"
        DEPENDS "${VKD3D_SOURCE}/libs/vkd3d-shader/${g}.y" VERBATIM)
    add_custom_command(OUTPUT "${GEN}/${g}.yy.c"
        COMMAND "${FLEX_EXE}" -o "${GEN}/${g}.yy.c" "${VKD3D_SOURCE}/libs/vkd3d-shader/${g}.l"
        DEPENDS "${VKD3D_SOURCE}/libs/vkd3d-shader/${g}.l" "${GEN}/${g}.tab.h" VERBATIM)
    list(APPEND GENERATED "${GEN}/${g}.tab.c" "${GEN}/${g}.yy.c")
endforeach()
file(GLOB VKD3D_SHADER_SOURCES "${VKD3D_SOURCE}/libs/vkd3d-shader/*.c")
add_library(vkd3d_shader STATIC ${VKD3D_SHADER_SOURCES} ${GENERATED}
    "${VKD3D_SOURCE}/libs/vkd3d-common/debug.c"
    "${VKD3D_SOURCE}/libs/vkd3d-common/error.c"
    "${VKD3D_SOURCE}/libs/vkd3d-common/memory.c"
    "${VKD3D_SOURCE}/libs/vkd3d-common/utf8.c")
target_include_directories(vkd3d_shader
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/inc" "${GEN}" "${VKD3D_SOURCE}/include/private" "${VKD3D_SOURCE}/libs/vkd3d-shader" "${VKD3D_SOURCE}"
    PUBLIC "${VKD3D_SOURCE}/include")
target_compile_definitions(vkd3d_shader PRIVATE LIBVKD3D_SHADER_SOURCE WINE_NO_NAMELESS_EXTENSION)
target_compile_options(vkd3d_shader PRIVATE -O2 -std=gnu11 -w -fPIC)
