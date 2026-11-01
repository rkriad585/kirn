cmake_minimum_required(VERSION 3.25)
project(kirn C CXX)   # C is required by find_package(LLVM)'s CheckIncludeFile
                      # on CMake >= 4, which fails without a C language enabled

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # feeds clangd/LSP

# std::thread lives in the CRT on MSVC but needs -pthread on POSIX link lines.
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(ZLIB REQUIRED)
find_package(SQLite3 REQUIRED)

# CMake 4's FindSQLite3 module can report SQLite3_FOUND without defining the
# imported target on some distro packages. Keep the link interface stable for
# both old and new CMake versions.
if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 UNKNOWN IMPORTED)
    set_target_properties(SQLite3::SQLite3 PROPERTIES
        IMPORTED_LOCATION "${SQLite3_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SQLite3_INCLUDE_DIRS}")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# KIRN_ASAN=ON instruments every target with AddressSanitizer (scripts/asanall.ps1).
# Phase 8: the old KIRN_ASAN cache slot is gone. Re-use its value once so an
# existing ASan build dir re-configures cleanly, then point the user at the
# new name (LLVM warn-and-correct pattern); do NOT keep reading KIRN_ASAN.
# Runs BEFORE option() so a stale -DKIRN_ASAN can still be seen/emigrated.
if(NOT CACHE{KIRN_ASAN} AND DEFINED KIRN_ASAN)
    set(KIRN_ASAN "${KIRN_ASAN}" CACHE BOOL "Build with AddressSanitizer" FORCE)
    message(DEPRECATION "KIRN_ASAN has been renamed to KIRN_ASAN; update your build command to -DKIRN_ASAN=ON")
endif()
option(KIRN_ASAN "Build with AddressSanitizer" OFF)
if(KIRN_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()

option(KIRN_TSAN "Build with ThreadSanitizer" OFF)
if(KIRN_TSAN)
    if(KIRN_ASAN)
        message(FATAL_ERROR "KIRN_ASAN and KIRN_TSAN cannot be enabled together")
    endif()
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
endif()

# Phase 4: opt-in LLVM codegen backend (-DKIRN_WITH_LLVM=ON). Off by default so
# a normal build stays dependency-free; the driver library never links LLVM --
# only kirnc does, behind KIRN_HAS_LLVM.
option(KIRN_WITH_LLVM "Build the LLVM MIR->IR backend and `kirnc llvm` verb" OFF)
option(KIRN_WITH_LLD "Link LLVM executables through the in-process LLD ELF driver" OFF)
set(KIRN_LLD_INCLUDE_DIR "" CACHE PATH
    "Directory containing lld/Common/Driver.h (from a matching LLD build)")
set(KIRN_LLD_LIBRARY_DIR "" CACHE PATH
    "Directory containing the matching lldELF and lldCommon libraries")

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

# HIR: a lowered, sugar-free IR over the checked AST (BUILDER-PLAN phase 3).
# It reads sema::Checker's resolved types, so it links kirn_sema rather than
# kirn_ast. Sits after sema and before the VM: the tree-walker and the bytecode
# compiler stay where they are, and this is a new plane beside them, not a
# replacement -- BUILDER-PLAN 4.5 requires the bytecode compiler to remain a
# separate specializer.
add_library(kirn_hir STATIC
    src/hir/hir.cpp
    src/hir/hir_verify.cpp
)
target_link_libraries(kirn_hir PUBLIC kirn_sema)

# MIR: a CFG-based, value-oriented IR built from HIR (BUILDER-PLAN phase 3).
# It links kirn_hir and deliberately NOT kirn_vm / kirn_interp: section 4.5 keeps
# the bytecode compiler a separate specializer that stays the runtime fast path,
# while MIR feeds AOT. Linking them together would make the two converge by
# accident, which is the thing the plan explicitly defers.
add_library(kirn_mir STATIC
    src/mir/mir.cpp
    src/mir/mir_verify.cpp
    src/mir/borrowck.cpp
)
target_link_libraries(kirn_mir PUBLIC kirn_hir)

add_library(kirn_vm STATIC
    src/vm/compiler.cpp
)
target_link_libraries(kirn_vm PUBLIC kirn_sema)

add_library(kirn_interp STATIC
    src/interp/runtime.cpp
)
# Threads::Threads propagates to every consumer (kirnrun, kirn) so POSIX
# link lines get their -pthread.
target_link_libraries(kirn_interp PUBLIC kirn_vm Threads::Threads OpenSSL::Crypto ZLIB::ZLIB SQLite3::SQLite3)

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
target_link_libraries(kirn_driver PUBLIC kirn_interp kirn_backend kirn_prepro kirn_hir kirn_mir)

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

if(KIRN_WITH_LLVM)
    find_package(LLVM REQUIRED CONFIG)
    add_library(kirn_llvm STATIC src/backends/llvm/emit_llvm.cpp)
    target_link_libraries(kirn_llvm PUBLIC kirn_mir)
    # LLVM's own headers stay out of the warnings budget.
    target_include_directories(kirn_llvm SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(kirn_llvm PRIVATE ${LLVM_DEFINITIONS})
    if(NOT KIRN_WITH_LLD)
        target_link_libraries(kirn_llvm PUBLIC LLVM)
    endif()
    target_compile_features(kirn_llvm PRIVATE cxx_std_20)

    target_link_libraries(kirnc PRIVATE kirn_llvm)
    target_compile_definitions(kirnc PRIVATE KIRN_HAS_LLVM=1)

    if(KIRN_WITH_LLD)
        if(NOT UNIX OR APPLE)
            message(FATAL_ERROR "KIRN_WITH_LLD currently supports ELF hosts only")
        endif()
        if(NOT KIRN_LLD_INCLUDE_DIR OR NOT KIRN_LLD_LIBRARY_DIR)
            message(FATAL_ERROR
                "KIRN_WITH_LLD requires KIRN_LLD_INCLUDE_DIR and KIRN_LLD_LIBRARY_DIR")
        endif()
        find_path(KIRN_LLD_DRIVER_INCLUDE lld/Common/Driver.h
            PATHS "${KIRN_LLD_INCLUDE_DIR}" NO_DEFAULT_PATH REQUIRED)
        find_library(KIRN_LLD_ELF_LIBRARY NAMES lldELF
            PATHS "${KIRN_LLD_LIBRARY_DIR}" NO_DEFAULT_PATH REQUIRED)
        find_library(KIRN_LLD_COMMON_LIBRARY NAMES lldCommon
            PATHS "${KIRN_LLD_LIBRARY_DIR}" NO_DEFAULT_PATH REQUIRED)
        find_library(KIRN_ZSTD_LIBRARY NAMES zstd REQUIRED)
        find_library(KIRN_LZMA_LIBRARY NAMES lzma REQUIRED)
        find_file(KIRN_CRT1_FILE NAMES crt1.o PATHS /usr/lib /usr/lib64 /lib /lib64
            REQUIRED)
        find_file(KIRN_CRTI_FILE NAMES crti.o PATHS /usr/lib /usr/lib64 /lib /lib64
            REQUIRED)
        find_file(KIRN_CRTN_FILE NAMES crtn.o PATHS /usr/lib /usr/lib64 /lib /lib64
            REQUIRED)
        file(GLOB KIRN_LLVM_STATIC_LIBS "${LLVM_LIBRARY_DIRS}/libLLVM*.a")
        target_include_directories(kirnc SYSTEM PRIVATE ${KIRN_LLD_DRIVER_INCLUDE})
        target_compile_definitions(kirnc PRIVATE KIRN_HAS_LLD=1
            KIRN_LLD_CRT1_PATH="${KIRN_CRT1_FILE}"
            KIRN_LLD_CRTI_PATH="${KIRN_CRTI_FILE}"
            KIRN_LLD_CRTN_PATH="${KIRN_CRTN_FILE}")
        # LLD is a static library and its LLVM dependencies are cyclic. Keep
        # the complete matching LLVM component set in one linker group so the
        # integration remains correct across LLVM component graph changes.
        target_link_libraries(kirnc PRIVATE "-Wl,--start-group"
            ${KIRN_LLD_ELF_LIBRARY} ${KIRN_LLD_COMMON_LIBRARY}
            ${KIRN_LLVM_STATIC_LIBS} ${KIRN_ZSTD_LIBRARY} ${KIRN_LZMA_LIBRARY}
            "-Wl,--end-group")
    endif()
endif()

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

# Installed distributions intentionally expose only the two user-facing tools.
# The legacy stage binaries remain build-tree compatibility fixtures for the
# corpus, but are not part of the shipped toolchain.
install(TARGETS kirn kirnc
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION bin
)
install(DIRECTORY stdpet DESTINATION .)
install(DIRECTORY src DESTINATION .)
install(FILES tools/runner.cpp DESTINATION tools)
install(FILES README.md DESTINATION .)

# kirn_driver is in the list so the refactored code keeps the same warning
# budget as the libraries around it; leaving it out would have exempted exactly
# the code being moved from -Wall -Wextra -Wpedantic.
foreach(tgt kirn_lex kirn_ast kirn_prepro kirn_parser kirn_sema kirn_hir kirn_mir kirn_vm kirn_interp kirn_backend kirn_driver)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

# The LLVM backend shares the warnings budget of the other kirn sources once it
# exists; its LLVM headers are still SYSTEM so LLVM's warnings don't price in.
if(TARGET kirn_llvm)
    if(MSVC)
        target_compile_options(kirn_llvm PRIVATE /W4 /permissive-)
    else()
        target_compile_options(kirn_llvm PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
