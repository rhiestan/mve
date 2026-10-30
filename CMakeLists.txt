# CMake build for the MVE libraries, command-line tools and unit tests.
# The Qt GUI (apps/umve) and the OpenGL library (libs/ogl) are not built.
# The existing Makefiles remain the reference build for Linux/macOS.

cmake_minimum_required(VERSION 3.21)
project(mve LANGUAGES CXX)

option(MVE_BUILD_APPS  "Build the command-line applications" ON)
option(MVE_BUILD_TESTS "Build the unit tests (requires GTest)" ON)
option(MVE_UNSAFE_MATH "Use -funsafe-math-optimizations (as the Makefiles do)" ON)
option(MVE_NATIVE_ARCH "Optimize for the build machine (-march=native)" OFF)
option(MVE_USE_OPENMP  "Use OpenMP if available" ON)
option(MVE_USE_HIGHWAY "Use Google Highway (runtime-dispatched SIMD) in fssrecon" ON)
option(MVE_USE_MIMALLOC "Use mimalloc for C++ allocations (operator new/delete)" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
# Same as the Makefiles: -O3 and no -DNDEBUG (asserts stay enabled). Debug
# information is only needed for profiling: configure with
# -DCMAKE_CXX_FLAGS_RELEASE="-O3 -g".
set(CMAKE_CXX_FLAGS_RELEASE "-O3" CACHE STRING "")

find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(TIFF REQUIRED)
find_package(Threads REQUIRED)
if(MVE_USE_HIGHWAY)
    find_package(hwy CONFIG REQUIRED)
endif()
if(MVE_USE_MIMALLOC)
    find_package(mimalloc CONFIG REQUIRED)
    if(TARGET mimalloc-static)
        set(MVE_MIMALLOC_TARGET mimalloc-static)
    else()
        set(MVE_MIMALLOC_TARGET mimalloc)
    endif()
endif()
if(MVE_USE_OPENMP)
    if(WIN32)
        # Prefer LLVM's own libomp.lib/libomp.dll (redistributable) over the one
        # shipped with Visual Studio, which imports libomp140.x86_64.dll.
        get_filename_component(llvm_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        if(EXISTS "${llvm_bin_dir}/../lib/libomp.lib")
            get_filename_component(llvm_omp_lib "${llvm_bin_dir}/../lib/libomp.lib" ABSOLUTE)
            set(OpenMP_libomp_LIBRARY "${llvm_omp_lib}" CACHE FILEPATH "" FORCE)
        endif()
    endif()
    find_package(OpenMP COMPONENTS CXX)
endif()

# Flags shared by all targets.
add_library(mve_options INTERFACE)
target_include_directories(mve_options INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/libs)
target_compile_options(mve_options INTERFACE
    -Wall -Wextra -Wundef -pedantic
    -fno-math-errno)
if(MVE_UNSAFE_MATH)
    target_compile_options(mve_options INTERFACE -funsafe-math-optimizations)
else()
    target_compile_options(mve_options INTERFACE -ffp-contract=off)
endif()
if(MVE_NATIVE_ARCH)
    target_compile_options(mve_options INTERFACE -march=native)
endif()
if(WIN32)
    target_compile_definitions(mve_options INTERFACE
        _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES
        _SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING)
endif()
if(APPLE)
    target_compile_definitions(mve_options INTERFACE GL_SILENCE_DEPRECATION=1)
endif()
target_link_libraries(mve_options INTERFACE Threads::Threads)
if(OpenMP_CXX_FOUND)
    target_link_libraries(mve_options INTERFACE OpenMP::OpenMP_CXX)
endif()

# Replaces global operator new/delete with mimalloc in an executable.
function(mve_use_mimalloc target)
    if(MVE_USE_MIMALLOC)
        target_sources(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/mimalloc_override.cc)
        target_link_libraries(${target} PRIVATE ${MVE_MIMALLOC_TARGET})
    endif()
endfunction()

# Libraries. Files starting with '_' are manual test programs.
function(mve_add_library name dir)
    file(GLOB sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/libs/${dir}/[^_]*.cc)
    add_library(${name} STATIC ${sources})
    target_link_libraries(${name} PUBLIC mve_options)
endfunction()

mve_add_library(mve_util util)
# libs/math is header-only.
add_library(mve_math INTERFACE)
target_link_libraries(mve_math INTERFACE mve_options)
mve_add_library(mve       mve)
mve_add_library(mve_sfm   sfm)
mve_add_library(mve_dmrecon dmrecon)
mve_add_library(mve_fssr  fssr)

target_link_libraries(mve PUBLIC mve_util mve_math JPEG::JPEG PNG::PNG TIFF::TIFF)
target_link_libraries(mve_sfm PUBLIC mve)
target_link_libraries(mve_dmrecon PUBLIC mve)
target_link_libraries(mve_fssr PUBLIC mve)
if(MVE_USE_HIGHWAY)
    target_link_libraries(mve_fssr PRIVATE hwy::hwy)
    target_compile_definitions(mve_fssr PRIVATE FSSR_USE_HIGHWAY=1)
endif()

# Applications.
if(MVE_BUILD_APPS)
    set(MVE_APPS
        bundle2pset dmrecon makescene meshconvert scene2pset sfmrecon
        featurerecon fssrecon mesh2pset meshalign meshclean sceneupgrade prebundle)
    set(app_libs_bundle2pset mve)
    set(app_libs_dmrecon     mve_dmrecon)
    set(app_libs_makescene   mve)
    set(app_libs_meshconvert mve)
    set(app_libs_scene2pset  mve)
    set(app_libs_sfmrecon    mve_sfm)
    set(app_libs_featurerecon mve_sfm)
    set(app_libs_fssrecon    mve_fssr)
    set(app_libs_mesh2pset   mve_fssr)
    set(app_libs_meshalign   mve_fssr)
    set(app_libs_meshclean   mve_fssr)
    set(app_libs_sceneupgrade mve_sfm)
    set(app_libs_prebundle   mve_sfm)
    foreach(app IN LISTS MVE_APPS)
        file(GLOB sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/apps/${app}/[^_]*.cc)
        add_executable(${app} ${sources})
        target_link_libraries(${app} PRIVATE ${app_libs_${app}})
        mve_use_mimalloc(${app})
        install(TARGETS ${app} RUNTIME DESTINATION bin)
    endforeach()
endif()

# Unit tests.
if(MVE_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*/gtest_*.cc)
    add_executable(mve_tests ${test_sources})
    target_link_libraries(mve_tests PRIVATE mve_fssr mve_sfm mve GTest::gtest_main)
    target_compile_options(mve_tests PRIVATE -Wno-sign-compare)
    mve_use_mimalloc(mve_tests)
    add_test(NAME mve_tests COMMAND mve_tests)
endif()

# On Windows the LLVM OpenMP runtime is a DLL that has to ship with the tools.
if(WIN32 AND OpenMP_CXX_FOUND AND MVE_BUILD_APPS)
    get_filename_component(compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_file(MVE_LIBOMP_DLL libomp.dll HINTS "${compiler_dir}" NO_DEFAULT_PATH)
    if(MVE_LIBOMP_DLL)
        add_custom_target(mve_copy_libomp ALL
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MVE_LIBOMP_DLL}" "${CMAKE_BINARY_DIR}")
        add_dependencies(dmrecon mve_copy_libomp)
        install(FILES "${MVE_LIBOMP_DLL}" DESTINATION bin)
    else()
        message(WARNING "libomp.dll not found next to the compiler; copy it manually")
    endif()
endif()
