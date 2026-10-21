cmake_minimum_required(VERSION 3.16)
project(linkway VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)            # C++17: builds on RHEL 8's GCC 8
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)         # -std=c++17, not gnu++17
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)   # -O2 -g: fast, and debuggable by Knot
endif()

option(LINKWAY_ASAN   "AddressSanitizer"            OFF)
option(LINKWAY_UBSAN  "UndefinedBehaviorSanitizer"  OFF)
option(LINKWAY_TSAN   "ThreadSanitizer"             OFF)
option(LINKWAY_TESTS  "Build tests"                 ON)
option(LINKWAY_HARDEN "Security hardening: FORTIFY, stack protector, CET, full RELRO, PIE" ON)
option(LINKWAY_FUZZ   "libFuzzer targets (clang)"   OFF)
option(BUILD_SHARED_LIBS "Build liblinkway as a shared library" OFF)

include(GNUInstallDirs)
find_package(Threads REQUIRED)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)   # PIE executables, PIC library

# ---- hardening (every target in this directory tree) -------------------------
# The flags distributions use for security-sensitive packages.
# Off under sanitizers: they bring their own instrumentation.
if(LINKWAY_HARDEN AND NOT (LINKWAY_ASAN OR LINKWAY_UBSAN OR LINKWAY_TSAN))
    include(CheckPIESupported)
    check_pie_supported()
    set(_fortify 2)
    if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12) OR
       (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16))
        set(_fortify 3)   # also checks buffers whose size is only known at run time
    endif()
    add_compile_options(
        "$<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=${_fortify}>"   # needs optimisation
        -D_GLIBCXX_ASSERTIONS             # bounds-checked std::vector::operator[] etc.
        -fstack-protector-strong -fstack-clash-protection
        -Wformat -Werror=format-security)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        add_compile_options(-fcf-protection=full)   # Intel CET: shadow stack + indirect-branch tracking
    endif()
    add_link_options(-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack -Wl,--as-needed)
endif()

add_library(linkway
    src/error.cpp
    src/message.cpp
    src/crc32c.cpp
    src/event_loop.cpp
    src/stream_transport.cpp
    src/shm_transport.cpp
    src/durable.cpp
    src/bus.cpp)
add_library(linkway::linkway ALIAS linkway)
set_target_properties(linkway PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
target_include_directories(linkway PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# PUBLIC: the headers need C++17 (std::byte, inline variables), so every consumer
# must compile as C++17 too. CMAKE_CXX_STANDARD above is NOT exported; without
# this, a find_package(linkway) consumer on GCC 8 (default gnu++14) failed to
# compile the headers (found by building a consumer against the installed package).
target_compile_features(linkway PUBLIC cxx_std_17)
target_link_libraries(linkway PUBLIC Threads::Threads rt)   # rt: shm_open on older glibc
target_compile_options(linkway PRIVATE -Wall -Wextra -Wshadow -pedantic-errors)

# Sanitizers are PUBLIC: tests and examples must be instrumented too.
if(LINKWAY_ASAN)
    target_compile_options(linkway PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(linkway PUBLIC -fsanitize=address)
endif()
if(LINKWAY_UBSAN)
    target_compile_options(linkway PUBLIC -fsanitize=undefined -fno-sanitize-recover=all)
    target_link_options(linkway PUBLIC -fsanitize=undefined)
endif()
if(LINKWAY_TSAN)
    # TSan does not model standalone atomic_thread_fence, which the ring's
    # futex doorbell uses (Dekker handshake). GCC warns about it. TSan still
    # checks every acquire/release push/pop; the doorbell is verified by the
    # TLA+ model (spec/) and covered by a stress test instead.
    target_compile_options(linkway PUBLIC -fsanitize=thread $<$<CXX_COMPILER_ID:GNU>:-Wno-tsan>)
    target_link_options(linkway PUBLIC -fsanitize=thread)
endif()

add_subdirectory(examples)

# ---- install: consumers use find_package(linkway) or pkg-config -------------------
include(CMakePackageConfigHelpers)
install(TARGETS linkway EXPORT linkwayTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} NAMELINK_COMPONENT devel
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT devel)
install(DIRECTORY include/linkway DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT devel)
install(EXPORT linkwayTargets NAMESPACE linkway:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/linkway COMPONENT devel)
configure_package_config_file(cmake/linkwayConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/linkwayConfig.cmake
                              INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/linkway)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/linkwayConfigVersion.cmake COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/linkwayConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/linkwayConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/linkway COMPONENT devel)
configure_file(cmake/linkway.pc.in ${CMAKE_CURRENT_BINARY_DIR}/linkway.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/linkway.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig COMPONENT devel)

# ---- tests ----------------------------------------------------------------------
if(LINKWAY_TESTS)
    # The system GoogleTest if there is one (distribution builds are offline),
    # else download a pinned release.
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        # Pinned by COMMIT hash (v1.14.0), not by tag: a tag can be moved, a commit
        # id is the content's own hash, so a tampered repository fails the build.
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG f8d7d77c06936315286eb55f8de22cd23c188571)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
    enable_testing()
    add_subdirectory(tests)
    add_subdirectory(fuzz)
endif()
