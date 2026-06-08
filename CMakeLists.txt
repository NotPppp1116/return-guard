cmake_minimum_required(VERSION 3.20)

project(returnguard VERSION 0.3.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RETURNGUARD_WARNINGS_AS_ERRORS "Treat ReturnGuard warnings as errors" OFF)
option(RETURNGUARD_BUILD_TESTS "Build and register ReturnGuard tests" ON)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "LLVM_DIR: ${LLVM_DIR}")
message(STATUS "Clang_DIR: ${Clang_DIR}")

add_executable(returnguard
    src/main.cpp
    src/Options.cpp
    src/Frontend.cpp
    src/internal/Analyzer.cpp
    src/internal/AnalyzerCalls.cpp
    src/internal/AnalyzerControlFlow.cpp
    src/internal/AnalyzerDiagnostics.cpp
    src/internal/AnalyzerDomains.cpp
    src/internal/AnalyzerParents.cpp
    src/internal/AnalyzerSource.cpp
    src/internal/AstUtils.cpp
    src/internal/ConditionEvaluator.cpp
    src/internal/DomainUtils.cpp
    src/internal/HandlerFinder.cpp
    src/internal/IfChain.cpp
    src/internal/ReturnCollector.cpp
)

target_include_directories(returnguard
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
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

if(RETURNGUARD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
