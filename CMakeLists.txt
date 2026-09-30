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

add_library(kirn_prepro STATIC
    src/prepro/prepro.cpp
)
# Consumes Token/DiagEngine and returns a Token stream, so it sits between the
# lexer and the parser. Linking kirn_lex (rather than kirn_support directly)
# means the preprocessor can never drift from the lexer's token vocabulary: if
# Tok gains an enumerator, this library sees it through the same header.
target_link_libraries(kirn_prepro PUBLIC kirn_lex)

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

# The driver core (BUILDER-PLAN.md phase 0). One translation unit on purpose:
# the code was moved out of tools/kirn.cpp's anonymous namespace, and an unnamed
# namespace has internal linkage per translation unit, so keeping it whole is
# what preserves linkage -- and therefore behavior -- across the split. `kirn`
# and the future `kirnc` both link this one implementation.
#
# PUBLIC link deps: the driver header exposes kirn::DiagEngine and
# kirn::ast::StmtP, and the implementation calls the lexer, parser, checker,
# interpreter, and native backend.
add_library(kirn_driver STATIC
    src/driver/driver.cpp
)
target_link_libraries(kirn_driver PUBLIC kirn_interp kirn_backend kirn_prepro)

# Phase 1: the stage drivers are thin front ends over the shared library, so all
# three link kirn_driver instead of the narrow lexer/parser/sema libs. They stay
# real executables rather than CMake ALIAS targets because an ALIAS is not a
# file on disk -- scripts/golden_linux.sh preflights `[ -x build/kirnlex ]`, and
# CI, editors and user scripts invoke these names directly.
add_executable(kirnlex tools/kirnlex.cpp)
target_link_libraries(kirnlex PRIVATE kirn_driver)

add_executable(kirnparse tools/kirnparse.cpp)
target_link_libraries(kirnparse PRIVATE kirn_driver)

add_executable(kirncheck tools/kirncheck.cpp)
target_link_libraries(kirncheck PRIVATE kirn_driver)

add_executable(kirnrun tools/kirnrun.cpp)
# kirnrun resolves modules through the driver library's libDirsFor, the same
# ordered list `kirn run` uses (phase 0 §5.2: one resolver, not two).
target_link_libraries(kirnrun PRIVATE kirn_driver)

# Phase 1: the compiler driver proper. Two lines, because phase 0 already
# extracted the pipeline into kirn_driver -- that library IS the point of the
# split, and it is what makes this target a 2-line addition instead of a
# re-implementation.
add_executable(kirnc tools/kirnc.cpp)
target_link_libraries(kirnc PRIVATE kirn_driver)

add_executable(kirn tools/kirn.cpp)
# ws2_32 is the WinSock import lib: only the `kirn doc` HTTP server in
# tools/kirn.cpp uses it, so it stays on the executable. The driver library
# deliberately does not link it, which keeps ws2_32 from propagating into every
# future consumer of kirn_driver (tests, kirnc, embedders).
if(WIN32)
    target_link_libraries(kirn PRIVATE kirn_driver ws2_32)
else()
    target_link_libraries(kirn PRIVATE kirn_driver)
endif()

# kirn_driver is in the list so the refactored code keeps the same warning
# budget as the libraries around it; leaving it out would have exempted exactly
# the code being moved from -Wall -Wextra -Wpedantic.
foreach(tgt kirn_lex kirn_ast kirn_prepro kirn_parser kirn_sema kirn_vm kirn_interp kirn_backend kirn_driver)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
