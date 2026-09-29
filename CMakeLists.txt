cmake_minimum_required(VERSION 3.25)
project(kirn CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # feeds clangd/LSP

# std::thread lives in the CRT on MSVC but needs -pthread on POSIX link lines.
find_package(Threads REQUIRED)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# KIRN_ASAN=ON instruments every target with AddressSanitizer (scripts/asanall.ps1).
# Phase 8: the old COCO_ASAN cache slot is gone. Re-use its value once so an
# existing ASan build dir re-configures cleanly, then point the user at the
# new name (LLVM warn-and-correct pattern); do NOT keep reading COCO_ASAN.
# Runs BEFORE option() so a stale -DCOCO_ASAN can still be seen/emigrated.
if(NOT CACHE{KIRN_ASAN} AND DEFINED COCO_ASAN)
    set(KIRN_ASAN "${COCO_ASAN}" CACHE BOOL "Build with AddressSanitizer" FORCE)
    message(DEPRECATION "COCO_ASAN has been renamed to KIRN_ASAN; update your build command to -DKIRN_ASAN=ON")
endif()
option(KIRN_ASAN "Build with AddressSanitizer" OFF)
if(KIRN_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()

add_library(kirn_support INTERFACE)
target_include_directories(kirn_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(kirn_lex STATIC
    src/lex/lexer.cpp
)
target_link_libraries(kirn_lex PUBLIC kirn_support)

add_library(kirn_ast STATIC
    src/ast/ast.cpp
    src/ast/ast_dump.cpp
)
target_link_libraries(kirn_ast PUBLIC kirn_support)

add_library(kirn_parser STATIC
    src/parser/parser.cpp
)
target_link_libraries(kirn_parser PUBLIC kirn_ast kirn_lex)

add_library(kirn_sema STATIC
    src/sema/checker.cpp
    src/sema/borrow.cpp
)
target_link_libraries(kirn_sema PUBLIC kirn_parser)

add_library(kirn_vm STATIC
    src/vm/compiler.cpp
)
target_link_libraries(kirn_vm PUBLIC kirn_sema)

add_library(kirn_interp STATIC
    src/interp/runtime.cpp
)
# Threads::Threads propagates to every consumer (kirnrun, kirn) so POSIX
# link lines get their -pthread.
target_link_libraries(kirn_interp PUBLIC kirn_vm Threads::Threads)

add_library(kirn_backend STATIC
    src/backend/native.cpp
)
target_link_libraries(kirn_backend PUBLIC kirn_sema)

add_executable(kirnlex tools/kirnlex.cpp)
target_link_libraries(kirnlex PRIVATE kirn_lex)

add_executable(kirnparse tools/kirnparse.cpp)
target_link_libraries(kirnparse PRIVATE kirn_parser)

add_executable(kirncheck tools/kirncheck.cpp)
target_link_libraries(kirncheck PRIVATE kirn_sema)

add_executable(kirnrun tools/kirnrun.cpp)
target_link_libraries(kirnrun PRIVATE kirn_interp)

add_executable(kirn tools/kirn.cpp)
# ws2_32 is the WinSock import lib: Windows-cmake only (the kirn driver bakes
# in a TCP network client). On POSIX hosts linking it fails, so gate it.
if(WIN32)
    target_link_libraries(kirn PRIVATE kirn_interp kirn_backend ws2_32)
else()
    target_link_libraries(kirn PRIVATE kirn_interp kirn_backend)
endif()

foreach(tgt kirn_lex kirn_ast kirn_parser kirn_sema kirn_vm kirn_interp kirn_backend)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
