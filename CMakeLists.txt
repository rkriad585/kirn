cmake_minimum_required(VERSION 3.25)
project(coco CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # feeds clangd/LSP

# std::thread lives in the CRT on MSVC but needs -pthread on POSIX link lines.
find_package(Threads REQUIRED)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# COCO_ASAN=ON instruments every target with AddressSanitizer (scripts/asanall.ps1).
option(COCO_ASAN "Build with AddressSanitizer" OFF)
if(COCO_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()

add_library(coco_support INTERFACE)
target_include_directories(coco_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(coco_lex STATIC
    src/lex/lexer.cpp
)
target_link_libraries(coco_lex PUBLIC coco_support)

add_library(coco_ast STATIC
    src/ast/ast.cpp
    src/ast/ast_dump.cpp
)
target_link_libraries(coco_ast PUBLIC coco_support)

add_library(coco_parser STATIC
    src/parser/parser.cpp
)
target_link_libraries(coco_parser PUBLIC coco_ast coco_lex)

add_library(coco_sema STATIC
    src/sema/checker.cpp
    src/sema/borrow.cpp
)
target_link_libraries(coco_sema PUBLIC coco_parser)

add_library(coco_vm STATIC
    src/vm/compiler.cpp
)
target_link_libraries(coco_vm PUBLIC coco_sema)

add_library(coco_interp STATIC
    src/interp/runtime.cpp
)
# Threads::Threads propagates to every consumer (cocorun, coco) so POSIX
# link lines get their -pthread.
target_link_libraries(coco_interp PUBLIC coco_vm Threads::Threads)

add_library(coco_backend STATIC
    src/backend/native.cpp
)
target_link_libraries(coco_backend PUBLIC coco_sema)

add_executable(cocolex tools/cocolex.cpp)
target_link_libraries(cocolex PRIVATE coco_lex)

add_executable(cocoparse tools/cocoparse.cpp)
target_link_libraries(cocoparse PRIVATE coco_parser)

add_executable(cococheck tools/cococheck.cpp)
target_link_libraries(cococheck PRIVATE coco_sema)

add_executable(cocorun tools/cocorun.cpp)
target_link_libraries(cocorun PRIVATE coco_interp)

add_executable(coco tools/coco.cpp)
# ws2_32 is the WinSock import lib: Windows-cmake only (the coco driver bakes
# in a TCP network client). On POSIX hosts linking it fails, so gate it.
if(WIN32)
    target_link_libraries(coco PRIVATE coco_interp coco_backend ws2_32)
else()
    target_link_libraries(coco PRIVATE coco_interp coco_backend)
endif()

foreach(tgt coco_lex coco_ast coco_parser coco_sema coco_vm coco_interp coco_backend)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
