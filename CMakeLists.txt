cmake_minimum_required(VERSION 3.28)
project(KairoScheduler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KAIRO_SCHEDULER_BUILD_BENCHMARK "Build scheduler telemetry benchmark" ${PROJECT_IS_TOP_LEVEL})

add_library(KairoScheduler)
target_sources(KairoScheduler
    PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src
        FILES src/KairoScheduler.cppm
)

add_executable(KairoSchedulerSmoke tests/smoke.cpp)
target_link_libraries(KairoSchedulerSmoke PRIVATE KairoScheduler)

enable_testing()
add_test(NAME KairoSchedulerSmoke COMMAND KairoSchedulerSmoke)


if(KAIRO_SCHEDULER_BUILD_BENCHMARK)
    add_executable(KairoSchedulerBenchmark benchmarks/benchmark.cpp)
    target_link_libraries(KairoSchedulerBenchmark PRIVATE KairoScheduler)
endif()

# Kairo portfolio warning policy: Kairo-owned source warnings are build failures.
# Catch2's __COUNTER__ registration macro is the only bounded Clang 23 test
# exception; production targets receive no blanket warning suppression.
foreach(_kairo_target IN ITEMS KairoScheduler KairoSchedulerSmoke KairoSchedulerBenchmark)
    if(TARGET ${_kairo_target})
        set_property(TARGET ${_kairo_target} PROPERTY COMPILE_WARNING_AS_ERROR ON)
    endif()
endforeach()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 23)
    foreach(_kairo_test_target IN ITEMS KairoSchedulerSmoke)
        if(TARGET ${_kairo_test_target})
            target_compile_options(${_kairo_test_target} PRIVATE -Wno-c2y-extensions)
        endif()
    endforeach()
endif()
