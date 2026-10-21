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

