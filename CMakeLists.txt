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
