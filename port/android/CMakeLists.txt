# TTT2 Android: libmain.so = the recompiled game code (ppc/) + the PC runtime (port/src/runtime,
# unchanged, compiled against the Win32 layer in win32/) + Android replacements (runtime/) +
# FFmpeg XMA (ffmpeg/) + SDL2. Built with the Android NDK for arm64-v8a:
#   cmake -S port/android -B <build> -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake
#         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release
#         -DFFMPEG_SRC=... -DFFMPEG_BUILD=... -DSDL2_SOURCE=... -DDXVK_NATIVE_SOURCE=...
cmake_minimum_required(VERSION 3.22)
project(ttt2_android C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_filename_component(PORT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
get_filename_component(ROOT "${PORT}/.." ABSOLUTE)
set(PPC_DIR     "${ROOT}/re/ppc")
set(XENON_UTILS "${ROOT}/tools/XenonRecomp/XenonUtils")
set(SIMDE_DIR   "${ROOT}/tools/XenonRecomp/thirdparty/simde")
set(RUNTIME     "${PORT}/src/runtime")
set(SDL2_SOURCE "" CACHE PATH "SDL2 source tree (release-2.30.x)")
set(DXVK_NATIVE_SOURCE "" CACHE PATH "dxvk-native source tree (headers: include/native)")
set(TTT2_D3D_LIBS "" CACHE STRING "DXVK-native d3d11/dxgi + D3DCompile libraries (stage 4); empty = the stage 3 stub")
foreach(v SDL2_SOURCE DXVK_NATIVE_SOURCE)
    if(NOT EXISTS "${${v}}")
        message(FATAL_ERROR "${v} is not set or missing: ${${v}}")
    endif()
endforeach()

# 1) recompiled game code
add_subdirectory(ppc)

# 2) FFmpeg (XMA decoder)
add_subdirectory(ffmpeg)

# 3) SDL2 (shared: SDLActivity loads libSDL2.so, then libmain.so)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${SDL2_SOURCE}" sdl2 EXCLUDE_FROM_ALL)

# 4) Win32 layer
set(WIN32_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/win32/include"
    "${DXVK_NATIVE_SOURCE}/include/native/windows"
    "${DXVK_NATIVE_SOURCE}/include/native/directx")
file(GLOB WIN32_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/win32/src/*.cpp")
add_library(ttt2_win32 STATIC ${WIN32_SOURCES})
target_include_directories(ttt2_win32 PRIVATE ${WIN32_INCLUDES} "${SIMDE_DIR}")
target_compile_options(ttt2_win32 PRIVATE -O2 -include msvc_crt.h -Wno-everything)
target_link_libraries(ttt2_win32 PRIVATE SDL2::SDL2)

# 5) libmain.so: the PC runtime + Android replacements
file(GLOB RUNTIME_KERNEL CONFIGURE_DEPENDS "${RUNTIME}/kernel/*.cpp")
list(FILTER RUNTIME_KERNEL EXCLUDE REGEX "/community_tls\\.cpp$")     # SChannel: replaced
set(RUNTIME_SOURCES
    "${RUNTIME}/main.cpp"
    "${RUNTIME}/runtime_math.cpp"
    "${RUNTIME}/guest_time.cpp"
    "${RUNTIME}/kernel_stubs.gen.cpp"
    ${RUNTIME_KERNEL}
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/android_main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/audio_output_android.cpp"   # XAudio2 -> AAudio
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/community_tls_android.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/abi_bridges.cpp")
if(NOT TTT2_D3D_LIBS)
    list(APPEND RUNTIME_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/runtime/d3d_unavailable.cpp")
endif()
add_library(main SHARED ${RUNTIME_SOURCES})
target_include_directories(main PRIVATE ${WIN32_INCLUDES} "${PPC_DIR}" "${XENON_UTILS}" "${SIMDE_DIR}" "${RUNTIME}" "${RUNTIME}/kernel")
target_compile_definitions(main PRIVATE
    TTT2_LOCAL_SAVES=0 TTT2_DRAW_CAPTURE_DIAGNOSTICS=1 TTT2_D3D11_MOVIE_BACKEND=1 TTT2_GPU_HW_BACKEND=1 TTT2_HAVE_STEAM=0
    SIMDE_ENABLE_NATIVE_ALIASES)
set_property(SOURCE "${RUNTIME}/main.cpp" APPEND PROPERTY COMPILE_DEFINITIONS TTT2_VERSION_STR="0.2.0-android" main=ttt2_main)
target_compile_options(main PRIVATE -O2 -include msvc_crt.h -Wno-everything -ffp-contract=off -fno-strict-aliasing -fno-autolink)
target_link_libraries(main PRIVATE tekken_ppc ttt2_ffmpeg ttt2_win32 SDL2::SDL2 ${TTT2_D3D_LIBS} aaudio log android dl m)
# #pragma comment(lib, "ws2_32.lib") & co. become ELF dependent-library records: ignore them.
target_link_options(main PRIVATE -Wl,--gc-sections -Wl,--no-undefined -Wl,--no-dependent-libraries -Wl,-z,max-page-size=16384)
