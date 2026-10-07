cmake_minimum_required(VERSION 3.20)
project(thumb CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(THUMB_DEV "Developer build: OBB fetch over adb reverse, guest sampler. Never release." OFF)

# --- dynarmic (ARM32 JIT) ---
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/dynarmic EXCLUDE_FROM_ALL)

# --- TLSF allocator (guest heap) ---
add_library(tlsf STATIC third_party/tlsf/tlsf.c)
target_include_directories(tlsf PUBLIC third_party/tlsf)

# --- libffi (Java -> guest native trampolines) ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFI REQUIRED IMPORTED_TARGET libffi)

# --- host32 core ---
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS core/*.cpp)
add_library(host32 STATIC ${CORE_SOURCES})
target_include_directories(host32 PUBLIC core third_party/jni)
target_link_libraries(host32 PUBLIC dynarmic tlsf PkgConfig::FFI z)
if (ANDROID)
    target_link_libraries(host32 PUBLIC log GLESv1_CM GLESv2 EGL android jnigraphics)
else()
    target_compile_definitions(host32 PUBLIC H32_HOST_LINUX=1)
endif()
target_compile_options(host32 PRIVATE -Wall -Wextra -Wno-unused-parameter)
if (THUMB_DEV)
    target_compile_definitions(host32 PUBLIC THUMB_DEV=1)
endif()

# --- Android: runtime + per-library stub ---
if (ANDROID)
    add_library(thumb SHARED shim/runtime.cpp shim/obb_handover.cpp shim/overlay.cpp)
    target_link_libraries(thumb PRIVATE host32 dl)
    add_library(thumb_stub SHARED shim/stub.cpp)
    target_link_libraries(thumb_stub PRIVATE log dl)
endif()

# --- Linux test harness ---
if (NOT ANDROID)
    add_executable(harness harness/main.cpp harness/fake_jni.cpp)
    target_link_libraries(harness PRIVATE host32)
endif()
