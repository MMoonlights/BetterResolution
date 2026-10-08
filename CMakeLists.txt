cmake_minimum_required(VERSION 3.20)
set(BR_VERSION "1.0.0" CACHE STRING "BetterResolution version")
project(BetterResolution VERSION "${BR_VERSION}" LANGUAGES C CXX)

option(BR_BUILD_TESTS "Build tests" ON)
option(BR_BUILD_TOOLS "Build the br command-line tool" ON)
option(BR_ENABLE_WINDOWS_CAPTURE "Windows capture backends (DXGI, WGC, GDI)" ON)
option(BR_STATIC_RUNTIME "Link the C/C++ runtime statically (MinGW/MSVC)" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(BR_SOURCES
    src/text/vision.cpp
    src/text/text.cpp
    src/api/api_core.cpp
    src/api/api_resize.cpp
    src/api/api_codec.cpp
    src/api/api_capture.cpp
    src/api/api_shot.cpp
    src/platform/capture_registry.cpp
    src/core/common.cpp
    src/core/cpu.cpp
    src/core/frame.cpp
    src/core/io.cpp
    src/core/thread_pool.cpp
    src/resize/kernels.cpp
    src/resize/sampler.cpp
    src/resize/cpu_resizer.cpp
    src/resize/resize_fixed.cpp
    src/resize/resize_fixed_avx2.cpp
    src/resize/resize_simd_scalar.cpp
    src/resize/resize_simd_generic.cpp
    src/resize/resize_simd_avx2.cpp
    src/analysis/delta.cpp
    src/analysis/diff.cpp
    src/container/brf.cpp
    src/codecs/codec_registry.cpp
    src/codecs/deflate.cpp
    src/codecs/huffman.cpp
    src/codecs/jpeg.cpp
    src/codecs/jpeg_simd.cpp
    src/codecs/jpeg_simd_avx2.cpp
    src/codecs/palette.cpp
    src/codecs/png.cpp
    src/codecs/png_filters.cpp
    src/codecs/png_filters_avx2.cpp
    src/codecs/simple_formats.cpp
    src/draw/draw.cpp
)

if(WIN32 AND BR_ENABLE_WINDOWS_CAPTURE)
    list(APPEND BR_SOURCES
        src/platform/windows/win_common.cpp
        src/platform/windows/enum.cpp
        src/platform/windows/capture_win.cpp
        src/platform/windows/dxgi_capture.cpp
        src/platform/windows/wgc_capture.cpp
        src/platform/windows/gdi_capture.cpp
        src/platform/windows/d3d11_scaler.cpp
    )
else()
    list(APPEND BR_SOURCES src/platform/capture_stub.cpp)
endif()

function(br_configure_target t)
    target_include_directories(${t} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/generated)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /permissive- /EHsc /utf-8)
        if(BR_STATIC_RUNTIME)
            set_property(TARGET ${t} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
    else()
        target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
        if(MINGW AND BR_STATIC_RUNTIME)
            target_link_options(${t} PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
    endif()
    if(WIN32)
        target_compile_definitions(${t} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
endfunction()

find_package(Threads REQUIRED)
set(BR_SYSTEM_LIBS Threads::Threads)
if(WIN32)
    list(APPEND BR_SYSTEM_LIBS user32 gdi32)
endif()

set(BR_SHARED_SOURCES ${BR_SOURCES})
if(WIN32)
    set(BR_RESOURCE_FILETYPE VFT_DLL)
    set(BR_RESOURCE_DESCRIPTION "BetterResolution image/screen processing library")
    set(BR_RESOURCE_INTERNAL_NAME betterresolution)
    set(BR_RESOURCE_FILENAME betterresolution.dll)
    configure_file(src/betterresolution.rc.in ${CMAKE_CURRENT_BINARY_DIR}/betterresolution.rc @ONLY)
    list(APPEND BR_SHARED_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/betterresolution.rc)
endif()
add_library(betterresolution SHARED ${BR_SHARED_SOURCES})
br_configure_target(betterresolution)
target_compile_definitions(betterresolution PRIVATE BR_BUILD_DLL=1)
target_compile_definitions(betterresolution PUBLIC
    BR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    BR_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    BR_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    BR_VERSION_STRING=\"${PROJECT_VERSION}\")
target_link_libraries(betterresolution PRIVATE ${BR_SYSTEM_LIBS})
set_target_properties(betterresolution PROPERTIES
    OUTPUT_NAME betterresolution
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
if(WIN32)
    set_target_properties(betterresolution PROPERTIES PREFIX "")
endif()
if(MINGW)
    target_link_options(betterresolution PRIVATE "-Wl,--output-def,${CMAKE_CURRENT_BINARY_DIR}/betterresolution.def")
    find_program(BR_DLLTOOL NAMES ${CMAKE_SYSTEM_PROCESSOR}-w64-mingw32-dlltool dlltool)
    if(BR_DLLTOOL)
        add_custom_command(TARGET betterresolution POST_BUILD
            COMMAND ${BR_DLLTOOL} -d ${CMAKE_CURRENT_BINARY_DIR}/betterresolution.def -D betterresolution.dll
                    -l ${CMAKE_CURRENT_BINARY_DIR}/betterresolution.lib
            COMMENT "Generating MSVC import library betterresolution.lib")
    endif()
endif()
add_library(BetterResolution::BetterResolution ALIAS betterresolution)

add_library(betterresolution_static STATIC ${BR_SOURCES})
br_configure_target(betterresolution_static)
target_compile_definitions(betterresolution_static PUBLIC BR_STATIC=1)
target_link_libraries(betterresolution_static PUBLIC ${BR_SYSTEM_LIBS})
add_library(BetterResolution::Static ALIAS betterresolution_static)

if(BR_BUILD_TOOLS)
    add_executable(br tools/br.cpp tools/text_command.cpp tools/vision_command.cpp tools/json.cpp)
    br_configure_target(br)
    target_link_libraries(br PRIVATE betterresolution_static)
    if(WIN32)
        set(BR_RESOURCE_FILETYPE VFT_APP)
        set(BR_RESOURCE_DESCRIPTION "BetterResolution command-line tool")
        set(BR_RESOURCE_INTERNAL_NAME br)
        set(BR_RESOURCE_FILENAME br.exe)
        configure_file(src/betterresolution.rc.in ${CMAKE_CURRENT_BINARY_DIR}/br.rc @ONLY)
        target_sources(br PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/br.rc)
        target_link_libraries(br PRIVATE shell32)
    endif()
endif()

if(BR_BUILD_TESTS)
    enable_testing()
    add_executable(br-tests
        tests/test_vision.cpp
        tests/test_text.cpp
        tests/test_main.cpp
        tests/test_codecs.cpp
        tests/test_deflate.cpp
        tests/test_png_filters.cpp
        tests/test_jpeg_simd.cpp
        tests/test_resize.cpp
        tests/test_resize_fixed.cpp
        tests/test_analysis.cpp
        tests/test_api.cpp
        tests/test_shot.cpp
        tests/test_cpp_wrapper.cpp
    )
    br_configure_target(br-tests)
    target_link_libraries(br-tests PRIVATE betterresolution_static)
    add_test(NAME br-tests COMMAND br-tests)
    add_executable(br-c-smoke tests/c_smoke.c)
    set_target_properties(br-c-smoke PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
    target_link_libraries(br-c-smoke PRIVATE betterresolution)
    add_test(NAME br-c-smoke COMMAND br-c-smoke)
endif()

include(GNUInstallDirs)
install(TARGETS betterresolution betterresolution_static
    EXPORT BetterResolutionTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(MINGW)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/betterresolution.def DESTINATION ${CMAKE_INSTALL_LIBDIR} OPTIONAL)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/betterresolution.lib DESTINATION ${CMAKE_INSTALL_LIBDIR} OPTIONAL)
endif()
if(BR_BUILD_TOOLS)
    install(TARGETS br RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(EXPORT BetterResolutionTargets
    FILE BetterResolutionTargets.cmake
    NAMESPACE BetterResolution::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BetterResolution)
include(CMakePackageConfigHelpers)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/BetterResolutionConfig.cmake.in [=[
@PACKAGE_INIT@

include(CMakeFindDependencyMacro)
find_dependency(Threads)
include("${CMAKE_CURRENT_LIST_DIR}/BetterResolutionTargets.cmake")
check_required_components(BetterResolution)
]=])
configure_package_config_file(${CMAKE_CURRENT_BINARY_DIR}/BetterResolutionConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/BetterResolutionConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BetterResolution)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/BetterResolutionConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/BetterResolutionConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/BetterResolutionConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BetterResolution)
