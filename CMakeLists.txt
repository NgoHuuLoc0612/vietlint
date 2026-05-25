cmake_minimum_required(VERSION 3.22)

project(VietLint
    VERSION 1.0.3
    DESCRIPTION "Vietnamese coding convention linter"
    LANGUAGES CXX C
)

# ---------------------------------------------------------------------------
# C++20 baseline
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD          23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Build type default
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(VIETLINT_ENABLE_AVX2       "Enable AVX2 SIMD acceleration"        ON)
option(VIETLINT_ENABLE_CLANG      "Build Clang LibTooling integration"    OFF)
option(VIETLINT_ENABLE_ONNX       "Build ONNX Runtime ML layer"           OFF)
option(VIETLINT_ENABLE_TREE_SITTER "Build tree-sitter lexer integration"  OFF)

set(VIETLINT_TREE_SITTER_DIR "" CACHE PATH "tree-sitter installation directory (with include/tree_sitter/api.h)")
option(VIETLINT_ENABLE_PYTHON     "Build pybind11 Python bridge"          OFF)
option(VIETLINT_ENABLE_TESTS      "Build unit tests (Google Test)"        ON)
option(VIETLINT_ENABLE_SANITIZERS "Enable ASAN/UBSAN in debug builds"     OFF)
option(VIETLINT_STATIC_RUNTIME    "Link C++ runtime statically"           OFF)

set(VIETLINT_LLVM_DIR  "" CACHE PATH "LLVM/Clang installation directory")
set(VIETLINT_ONNX_DIR  "" CACHE PATH "ONNX Runtime installation directory")

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
add_library(vietlint_compile_opts INTERFACE)

if(MSVC)
    target_compile_options(vietlint_compile_opts INTERFACE
        /W4 /WX- /utf-8 /permissive- /Zc:__cplusplus
        $<$<CONFIG:Release>:/O2 /GL>
        $<$<CONFIG:Debug>:/Od /RTC1>
        $<$<BOOL:${VIETLINT_ENABLE_AVX2}>:/arch:AVX2>
    )
    target_compile_definitions(vietlint_compile_opts INTERFACE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    if(VIETLINT_STATIC_RUNTIME)
        set_property(TARGET vietlint_compile_opts PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
else()
    target_compile_options(vietlint_compile_opts INTERFACE
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
        $<$<CONFIG:Debug>:-O0 -g3>
        $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG>
        $<$<BOOL:${VIETLINT_ENABLE_AVX2}>:-mavx2 -mbmi2>
    )
    if(VIETLINT_ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(vietlint_compile_opts INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(vietlint_compile_opts INTERFACE
            -fsanitize=address,undefined
        )
    endif()
endif()

if(VIETLINT_ENABLE_AVX2)
    target_compile_definitions(vietlint_compile_opts INTERFACE __AVX2__=1)
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
file(GLOB CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
)
add_library(vietlint_core STATIC ${CORE_SOURCES})

target_include_directories(vietlint_core
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

set_target_properties(vietlint_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(vietlint_core
    PUBLIC  vietlint_compile_opts
)

# Thread support
find_package(Threads REQUIRED)
target_link_libraries(vietlint_core PUBLIC Threads::Threads dl)

# ---------------------------------------------------------------------------
# tree-sitter integration (optional)
# ---------------------------------------------------------------------------
if(VIETLINT_ENABLE_TREE_SITTER)
    if(VIETLINT_TREE_SITTER_DIR)
        find_library(TREE_SITTER_LIB
            NAMES tree-sitter
            PATHS "${VIETLINT_TREE_SITTER_DIR}/lib"
            NO_DEFAULT_PATH
        )
        set(TREE_SITTER_INCLUDE "${VIETLINT_TREE_SITTER_DIR}/include")
    else()
        # Try system install
        find_library(TREE_SITTER_LIB NAMES tree-sitter)
        find_path(TREE_SITTER_INCLUDE tree_sitter/api.h)
    endif()

    if(TREE_SITTER_LIB AND TREE_SITTER_INCLUDE)
        target_include_directories(vietlint_core PUBLIC ${TREE_SITTER_INCLUDE})
        target_link_libraries(vietlint_core PUBLIC ${TREE_SITTER_LIB})
        target_compile_definitions(vietlint_core PUBLIC HAVE_TREE_SITTER=1)
        message(STATUS "tree-sitter: ${TREE_SITTER_LIB}")
    else()
        message(WARNING "tree-sitter not found — TreeSitterLexer will use fallback mode. "
                        "Install with: sudo apt install libtree-sitter-dev  "
                        "or set VIETLINT_TREE_SITTER_DIR")
        # Still compiles — HAVE_TREE_SITTER=0 path is the fallback stub
    endif()

    # Set wasm grammar directory default (used at runtime)
    set(VIETLINT_GRAMMAR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/grammars"
        CACHE PATH "Directory containing tree-sitter-*.wasm grammar files")
    target_compile_definitions(vietlint_core PUBLIC
        VIETLINT_GRAMMAR_DIR="${VIETLINT_GRAMMAR_DIR}")
    message(STATUS "Grammar directory: ${VIETLINT_GRAMMAR_DIR}")
endif()

# ---------------------------------------------------------------------------
# LSP library
# ---------------------------------------------------------------------------
add_library(vietlint_lsp STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lsp/lsp_server.cpp"
)
target_include_directories(vietlint_lsp
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(vietlint_lsp
    PUBLIC  vietlint_core vietlint_compile_opts
)

# ---------------------------------------------------------------------------
# ML library
# ---------------------------------------------------------------------------
add_library(vietlint_ml STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ml/ml_classifier.cpp"
)
target_include_directories(vietlint_ml
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(vietlint_ml
    PUBLIC  vietlint_core vietlint_compile_opts
)

# ONNX Runtime integration
if(VIETLINT_ENABLE_ONNX)
    if(VIETLINT_ONNX_DIR)
        find_library(ONNX_RUNTIME_LIB
            NAMES onnxruntime
            PATHS "${VIETLINT_ONNX_DIR}/lib"
            NO_DEFAULT_PATH
        )
        if(ONNX_RUNTIME_LIB)
            target_include_directories(vietlint_ml PUBLIC
                "${VIETLINT_ONNX_DIR}/include"
            )
            target_link_libraries(vietlint_ml PUBLIC ${ONNX_RUNTIME_LIB})
            target_compile_definitions(vietlint_ml PUBLIC HAVE_ONNX_RUNTIME=1)
            message(STATUS "ONNX Runtime: ${ONNX_RUNTIME_LIB}")
        else()
            message(WARNING "ONNX Runtime library not found in ${VIETLINT_ONNX_DIR}")
        endif()
    else()
        # Try pkg-config
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(ONNXRUNTIME QUIET onnxruntime)
            if(ONNXRUNTIME_FOUND)
                target_include_directories(vietlint_ml PUBLIC ${ONNXRUNTIME_INCLUDE_DIRS})
                target_link_libraries(vietlint_ml PUBLIC ${ONNXRUNTIME_LIBRARIES})
                target_compile_definitions(vietlint_ml PUBLIC HAVE_ONNX_RUNTIME=1)
            endif()
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Clang LibTooling integration
# ---------------------------------------------------------------------------
if(VIETLINT_ENABLE_CLANG)
add_library(vietlint_clang STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/llvm/clang_visitor.cpp"
)
target_include_directories(vietlint_clang
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(vietlint_clang
    PUBLIC  vietlint_core vietlint_compile_opts
)
else()
add_library(vietlint_clang INTERFACE)
target_link_libraries(vietlint_clang INTERFACE vietlint_core)
endif()

if(VIETLINT_ENABLE_CLANG)
    if(VIETLINT_LLVM_DIR)
        list(APPEND CMAKE_PREFIX_PATH "${VIETLINT_LLVM_DIR}")
    endif()

    find_package(LLVM REQUIRED CONFIG)
    find_package(Clang REQUIRED CONFIG
        HINTS "${LLVM_DIR}/../clang" "${LLVM_DIR}/../lib/cmake/clang"
    )

    message(STATUS "LLVM version: ${LLVM_VERSION}")
    message(STATUS "Clang include dirs: ${CLANG_INCLUDE_DIRS}")

    target_include_directories(vietlint_clang SYSTEM PUBLIC
        ${LLVM_INCLUDE_DIRS}
        ${CLANG_INCLUDE_DIRS}
    )

    # Select required Clang/LLVM libs
    set(CLANG_LIBS
        clangTooling
        clangToolingCore
        clangFrontend
        clangFrontendTool
        clangDriver
        clangSerialization
        clangCodeGen
        clangParse
        clangSema
        clangAnalysis
        clangAST
        clangASTMatchers
        clangEdit
        clangLex
        clangBasic
        clangRewrite
    )

    # Optional: clang-tidy if available
    find_library(CLANG_TIDY_LIB clangTidy
        PATHS ${LLVM_LIBRARY_DIRS}
        NO_DEFAULT_PATH
    )
    if(CLANG_TIDY_LIB)
        list(APPEND CLANG_LIBS clangTidy clangTidyUtils)
        target_compile_definitions(vietlint_clang PUBLIC HAVE_CLANG_TIDY=1)
        message(STATUS "clang-tidy support: enabled")
    endif()

    target_link_libraries(vietlint_clang PUBLIC
        ${CLANG_LIBS}
        ${LLVM_AVAILABLE_LIBS}
    )
    target_compile_definitions(vietlint_clang PUBLIC HAVE_CLANG=1)

    llvm_map_components_to_libnames(LLVM_CORE_LIBS
        support core irreader option)
    target_link_libraries(vietlint_clang PUBLIC ${LLVM_CORE_LIBS})
endif()

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(vietlint
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
)
target_link_libraries(vietlint
    PRIVATE
    vietlint_core
    vietlint_lsp
    vietlint_ml
    vietlint_clang
    vietlint_compile_opts
)

# Install
install(TARGETS vietlint
    RUNTIME DESTINATION bin
)
install(TARGETS vietlint_core vietlint_lsp vietlint_ml
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.hpp"
)

# ---------------------------------------------------------------------------
# pybind11 extension module
# ---------------------------------------------------------------------------
if(VIETLINT_ENABLE_PYTHON)
    find_package(Python3 REQUIRED COMPONENTS Interpreter Development)

    include(FetchContent)
    FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG        v2.11.1
    )
    FetchContent_MakeAvailable(pybind11)

    pybind11_add_module(vietlint_core_py
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ml/pybind_bridge.cpp"
    )

    set_target_properties(vietlint_core_py PROPERTIES
        OUTPUT_NAME "vietlint_core"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/python"
    )

    target_include_directories(vietlint_core_py PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(vietlint_core_py PRIVATE
        vietlint_core vietlint_ml vietlint_lsp vietlint_clang
        vietlint_compile_opts
    )

    install(TARGETS vietlint_core_py
        LIBRARY DESTINATION "${Python3_SITEARCH}"
    )

    message(STATUS "Python bridge: enabled (Python ${Python3_VERSION})")
endif()

# ---------------------------------------------------------------------------
# Tests (Google Test)
# ---------------------------------------------------------------------------
if(VIETLINT_ENABLE_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    add_executable(vietlint_tests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_utf8_scanner.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lexer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_classifier.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_encoding_detector.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rule_engine.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lsp_server.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ml_features.cpp"
    )

    target_include_directories(vietlint_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(vietlint_tests
        PRIVATE
        vietlint_core
        vietlint_lsp
        vietlint_ml
        vietlint_clang
        vietlint_compile_opts
        GTest::gtest_main
        GTest::gmock
    )

    include(GoogleTest)
    gtest_discover_tests(vietlint_tests
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests"
        PROPERTIES TIMEOUT 60
    )

    message(STATUS "Tests: enabled")
endif()

# ---------------------------------------------------------------------------
# Plugin example (demonstrates the plugin API)
# ---------------------------------------------------------------------------
add_library(vietlint_plugin_example MODULE
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/example_plugin.cpp"
)
target_include_directories(vietlint_plugin_example PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(vietlint_plugin_example PRIVATE
    vietlint_core vietlint_compile_opts
)
set_target_properties(vietlint_plugin_example PROPERTIES
    PREFIX ""
    OUTPUT_NAME "vietlint_example_plugin"
)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "VietLint ${PROJECT_VERSION} configuration:")
message(STATUS "  Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "  AVX2 SIMD:      ${VIETLINT_ENABLE_AVX2}")
message(STATUS "  Clang tooling:  ${VIETLINT_ENABLE_CLANG}")
message(STATUS "  tree-sitter:    ${VIETLINT_ENABLE_TREE_SITTER}")
message(STATUS "  ONNX Runtime:   ${VIETLINT_ENABLE_ONNX}")
message(STATUS "  Python bridge:  ${VIETLINT_ENABLE_PYTHON}")
message(STATUS "  Unit tests:     ${VIETLINT_ENABLE_TESTS}")
message(STATUS "  Sanitizers:     ${VIETLINT_ENABLE_SANITIZERS}")
message(STATUS "")
