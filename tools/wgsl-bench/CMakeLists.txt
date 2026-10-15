cmake_minimum_required(VERSION 3.22)
project(bwsl_tint_benchmark LANGUAGES C CXX)

if(NOT DAWN_SOURCE_DIR)
    message(FATAL_ERROR "Set DAWN_SOURCE_DIR to the pinned Dawn checkout")
endif()

add_subdirectory("${DAWN_SOURCE_DIR}" dawn)

add_executable(bwsl-tint-bench tint_bench.cpp)
target_compile_features(bwsl-tint-bench PRIVATE cxx_std_20)
target_compile_definitions(bwsl-tint-bench PRIVATE
    BWSL_TINT_REVISION="${BWSL_TINT_REVISION}")
target_link_libraries(bwsl-tint-bench PRIVATE
    dawn_utils_config
    tint_api
    tint_api_helpers
    tint_lang_wgsl_inspector
    tint_lang_wgsl_reader
    tint_lang_spirv_writer)
