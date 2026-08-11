cmake_minimum_required(VERSION 3.20)
project(fpv_atv_hackrf_decoder CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
    add_compile_definitions(_USE_MATH_DEFINES NOMINMAX)
    add_compile_options(/fp:fast /arch/AVX2 /utf-8)
else()
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -ffast-math")
endif()

# libhackrf: pkg-config where available (Linux/macOS), otherwise search
# directly (Windows: PothosSDR ships hackrf.h + hackrf.lib).
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(HACKRF IMPORTED_TARGET libhackrf)
endif()
if(TARGET PkgConfig::HACKRF)
    set(HACKRF_TARGET PkgConfig::HACKRF)
else()
    find_path(HACKRF_INCLUDE_DIR libhackrf/hackrf.h
              HINTS "C:/Program Files/PothosSDR/include")
    find_library(HACKRF_LIBRARY NAMES hackrf
                 HINTS "C:/Program Files/PothosSDR/lib")
    if(NOT HACKRF_INCLUDE_DIR OR NOT HACKRF_LIBRARY)
        message(FATAL_ERROR "libhackrf not found (pkg-config or PothosSDR)")
    endif()
    add_library(hackrf_import INTERFACE)
    target_include_directories(hackrf_import INTERFACE ${HACKRF_INCLUDE_DIR})
    target_link_libraries(hackrf_import INTERFACE ${HACKRF_LIBRARY})
    set(HACKRF_TARGET hackrf_import)
endif()

# SDL2: a local copy under third_party/SDL2 (Windows) is found first.
find_package(SDL2 REQUIRED CONFIG
             HINTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL2")

# SoapySDR: optional, enables additional SDR device support (LibreSDR B220, etc.)
option(SOAPYSDR "Enable SoapySDR support for additional SDR devices" OFF)
if(SOAPYSDR)
    find_package(SoapySDR REQUIRED)
endif()

option(UHD "Enable native UHD support" OFF)
if(UHD)
    find_package(UHD REQUIRED)
endif()

add_library(fpv_dsp STATIC
    src/dsp/ntsc_decoder.cpp
)
target_include_directories(fpv_dsp PUBLIC src)

add_executable(fpvdec
    src/main.cpp
    src/source/hackrf_source.cpp
    src/ui/sdl_display.cpp
    src/ui/gui_manager.cpp
    src/ui/web_display.cpp
    src/filter/filter.cpp
    src/filter/denoiser.cpp
    src/filter/temporal.cpp
    src/filter/temporal_median.cpp
)

# WebGUI (Web-based display mode via cpp-httplib): allows headless server mode
# with a browser-based control panel. Requires cpp-httplib (vendored in third_party).
option(WEBGUI "Enable Web GUI mode (browser-based display via cpp-httplib)" OFF)
if(WEBGUI)
    message(STATUS "Web GUI mode enabled")
    target_include_directories(fpvdec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    find_package(Threads REQUIRED)
    pkg_check_modules(JPEG REQUIRED libturbojpeg libjpeg)
    target_link_libraries(fpvdec PRIVATE Threads::Threads ${JPEG_LIBRARIES})
    target_include_directories(fpvdec PRIVATE ${JPEG_INCLUDE_DIRS})
    target_compile_definitions(fpvdec PRIVATE HAVE_WEBGUI)
endif()

# Add native UHD source and dependencies if enabled
if(UHD)
    target_sources(fpvdec PRIVATE src/source/uhd_source.cpp)
    target_link_libraries(fpvdec PRIVATE ${UHD_LIBRARIES})
    target_include_directories(fpvdec PRIVATE ${UHD_INCLUDE_DIRS})
    target_compile_definitions(fpvdec PRIVATE HAVE_UHD)
endif()

# Add SoapySDR source and dependencies if enabled
if(SOAPYSDR)
    target_sources(fpvdec PRIVATE src/source/soapy_source.cpp)
    target_link_libraries(fpvdec PRIVATE ${SoapySDR_LIBRARIES})
    target_include_directories(fpvdec PRIVATE ${SoapySDR_INCLUDE_DIRS})
    target_compile_definitions(fpvdec PRIVATE HAVE_SOAPYSDR)
endif()

# ImGui (used only by gui_manager, not directly by main)
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)
target_include_directories(fpvdec PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_sources(fpvdec PRIVATE ${IMGUI_SOURCES})
target_link_libraries(fpvdec PRIVATE fpv_dsp ${HACKRF_TARGET} SDL2::SDL2)

if(WIN32)
    # SDL2's WinMain shim is not used; keep the plain main().
    target_compile_definitions(fpvdec PRIVATE SDL_MAIN_HANDLED)
    add_custom_command(TARGET fpvdec POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:fpvdec>)
    if(HACKRF_LIBRARY)
        get_filename_component(_hackrf_lib_dir "${HACKRF_LIBRARY}" DIRECTORY)
        if(EXISTS "${_hackrf_lib_dir}/../bin/hackrf.dll")
            add_custom_command(TARGET fpvdec POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_hackrf_lib_dir}/../bin/hackrf.dll"
                        $<TARGET_FILE_DIR:fpvdec>)
        endif()
    endif()
endif()

enable_testing()
add_executable(synth_fm tests/synth_fm.cpp)
target_link_libraries(synth_fm PRIVATE fpv_dsp)
add_test(NAME synth_fm COMMAND synth_fm)
