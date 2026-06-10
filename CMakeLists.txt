cmake_minimum_required(VERSION 3.20)

project(returnguard VERSION 0.7.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RETURNGUARD_WARNINGS_AS_ERRORS "Treat ReturnGuard warnings as errors" OFF)
option(RETURNGUARD_BUILD_TESTS "Build and register ReturnGuard tests" ON)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "LLVM_DIR: ${LLVM_DIR}")
message(STATUS "Clang_DIR: ${Clang_DIR}")

find_program(RETURNGUARD_CLANG_EXECUTABLE
    NAMES "clang-${LLVM_VERSION_MAJOR}" clang
    HINTS "${LLVM_TOOLS_BINARY_DIR}"
    NO_DEFAULT_PATH
)
if(NOT RETURNGUARD_CLANG_EXECUTABLE)
    find_program(RETURNGUARD_CLANG_EXECUTABLE
        NAMES "clang-${LLVM_VERSION_MAJOR}" clang
    )
endif()

set(RETURNGUARD_RESOURCE_DIR "")
if(RETURNGUARD_CLANG_EXECUTABLE)
    execute_process(
        COMMAND "${RETURNGUARD_CLANG_EXECUTABLE}" -print-resource-dir
        RESULT_VARIABLE RETURNGUARD_RESOURCE_RESULT
        OUTPUT_VARIABLE RETURNGUARD_RESOURCE_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(RETURNGUARD_RESOURCE_RESULT EQUAL 0
       AND IS_DIRECTORY "${RETURNGUARD_RESOURCE_OUTPUT}/include")
        file(TO_CMAKE_PATH
            "${RETURNGUARD_RESOURCE_OUTPUT}"
            RETURNGUARD_RESOURCE_DIR)
    endif()
endif()

if(NOT RETURNGUARD_RESOURCE_DIR)
    foreach(LLVM_LIBRARY_DIR IN LISTS LLVM_LIBRARY_DIRS)
        if(IS_DIRECTORY "${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}/include")
            file(TO_CMAKE_PATH
                "${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}"
                RETURNGUARD_RESOURCE_DIR)
            break()
        endif()
    endforeach()
endif()

if(RETURNGUARD_RESOURCE_DIR)
    message(STATUS "Clang resource directory: ${RETURNGUARD_RESOURCE_DIR}")
else()
    message(WARNING
        "Could not determine Clang's resource directory; builtin headers may "
        "require an explicit -resource-dir argument")
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/BuildConfig.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/returnguard/BuildConfig.hpp"
    @ONLY
)

add_executable(returnguard
    src/main.cpp
    src/CompilationSetup.cpp
    src/Options.cpp
    src/Frontend.cpp
    src/internal/Analyzer.cpp
    src/internal/AnalyzerCalls.cpp
    src/internal/AnalyzerControlFlow.cpp
    src/internal/AnalyzerDiagnostics.cpp
    src/internal/AnalyzerDomains.cpp
    src/internal/AnalyzerFlow.cpp
    src/internal/AnalyzerFlowControl.cpp
    src/internal/AnalyzerNullState.cpp
    src/internal/AnalyzerParents.cpp
    src/internal/AnalyzerSource.cpp
    src/internal/AstUtils.cpp
    src/internal/CFGValueFlow.cpp
    src/internal/ConditionEvaluator.cpp
    src/internal/DomainUtils.cpp
    src/internal/HandlerFinder.cpp
    src/internal/IfChain.cpp
    src/internal/NullStateAnalysis.cpp
    src/internal/ReturnCollector.cpp
    src/internal/ValueSetInference.cpp
)

target_include_directories(returnguard
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_include_directories(returnguard SYSTEM PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
)

target_compile_definitions(returnguard PRIVATE ${LLVM_DEFINITIONS})

target_compile_options(returnguard PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wno-unused-parameter
)

if(RETURNGUARD_WARNINGS_AS_ERRORS)
    target_compile_options(returnguard PRIVATE -Werror)
endif()

# Arch Linux and many other distributions expose a monolithic clang-cpp
# library. Source builds of LLVM may instead export component targets.
if(TARGET clang-cpp)
    target_link_libraries(returnguard PRIVATE clang-cpp)
elseif(TARGET clangTooling)
    target_link_libraries(returnguard PRIVATE
        clangTooling
        clangFrontend
        clangAST
        clangBasic
        clangLex
        clangParse
        clangSema
        clangAnalysis
        clangSerialization
    )
else()
    find_library(CLANG_CPP_LIBRARY
        NAMES clang-cpp
        HINTS ${LLVM_LIBRARY_DIRS}
    )

    if(NOT CLANG_CPP_LIBRARY)
        file(GLOB CLANG_CPP_CANDIDATES
            "${LLVM_LIBRARY_DIRS}/libclang-cpp.so*"
            "/usr/lib/libclang-cpp.so*"
            "/usr/local/lib/libclang-cpp.so*"
        )
        if(CLANG_CPP_CANDIDATES)
            list(SORT CLANG_CPP_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
            list(GET CLANG_CPP_CANDIDATES 0 CLANG_CPP_LIBRARY)
        endif()
    endif()

    if(NOT CLANG_CPP_LIBRARY)
        message(FATAL_ERROR "Could not locate libclang-cpp")
    endif()

    target_link_libraries(returnguard PRIVATE "${CLANG_CPP_LIBRARY}")
endif()

if(TARGET LLVM)
    target_link_libraries(returnguard PRIVATE LLVM)
else()
    llvm_map_components_to_libnames(LLVM_LIBS Support Option)
    target_link_libraries(returnguard PRIVATE ${LLVM_LIBS})
endif()

include(GNUInstallDirs)
install(TARGETS returnguard RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(
    FILES include/returnguard/Nullability.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/returnguard
)
install(
    PROGRAMS scripts/returnguard_project.py
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    RENAME returnguard-project
)
install(
    FILES scripts/returnguard_project_runtime.py
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(RETURNGUARD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
