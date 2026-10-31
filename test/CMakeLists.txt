find_package(PkgConfig REQUIRED)

pkg_check_modules(GTEST gtest REQUIRED)

macro(ut_executable name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} qumirdb ${GTEST_LIBRARIES})
    target_include_directories(${name} PRIVATE ${GTEST_INCLUDE_DIRS})
    target_link_directories(${name} PRIVATE ${GTEST_LIBRARY_DIRS})
    target_compile_options(${name} PRIVATE -Wno-character-conversion)
endmacro()

macro(ut name source)
    ut_executable(${name} ${source})
    add_test(NAME ${name} COMMAND ${name} ${ARGN} --gtest_output=xml:${CMAKE_CURRENT_BINARY_DIR}/${name}.xml)
endmacro()

find_program(QDB_PYTHON_EXECUTABLE python3)
if(QDB_PYTHON_EXECUTABLE)
    add_test(
        NAME test_benchmark_runner
        COMMAND ${QDB_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_benchmark_runner.py -v
    )
endif()

ut(test_io test_io.cpp)
ut(test_parquet_row_reader test_parquet_row_reader.cpp)
ut(test_stats test_stats.cpp)
ut(test_row_group_predicate test_row_group_predicate.cpp)
ut(test_row_group_pushdown_e2e test_row_group_pushdown_e2e.cpp)
ut(test_cbo test_cbo.cpp)
ut(test_sexp test_sexp.cpp)
ut(test_aggregate test_aggregate.cpp)
ut(test_aggregate_f64 test_aggregate_f64.cpp)
ut(test_aggregate_table test_aggregate_table.cpp)
ut(test_filter test_filter.cpp)
ut(test_string_types test_string_types.cpp)
ut(test_string_ops test_string_ops.cpp)
ut(test_swiss_group test_swiss_group.cpp)
ut(test_hash_upsert test_hash_upsert.cpp)
ut(test_string_key_descriptor test_string_key_descriptor.cpp)
ut(test_owned_blocks test_owned_blocks.cpp)
ut(test_join test_join.cpp)
ut(test_join_node test_join_node.cpp)
ut(test_join_key test_join_key.cpp)
ut(test_finalize_fused test_finalize_fused.cpp)
ut(test_project_type test_project_type.cpp)
ut(test_project_kernel test_project_kernel.cpp)
ut(test_kernel_spec test_kernel_spec.cpp)
ut(test_project_e2e test_project_e2e.cpp)
ut(test_external_module test_external_module.cpp)
ut(test_join_pipeline test_join_pipeline.cpp)
# TODO(remove-next): test_join_exec drove the deleted TRuntimeJoin pull executor
# directly (mock IRuntimeNode sources). Rewrite against TInnerJoinProcessor or the
# scheduler before re-enabling. Join coverage meanwhile: test_join_planner + TPC-H.
# ut(test_join_exec test_join_exec.cpp)
ut(test_join_kernel test_join_kernel.cpp)
ut(test_join_planner test_join_planner.cpp)
ut(test_join_asymmetric test_join_asymmetric.cpp)
ut(test_runtime_filter test_runtime_filter.cpp)
ut(test_lexer test_lexer.cpp)
find_package(LLVM REQUIRED CONFIG)
add_library(regtest_lib STATIC regtest_lib.cpp)
target_link_libraries(regtest_lib PUBLIC qumirdb)
target_include_directories(regtest_lib PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(regtest_lib PRIVATE -Wno-character-conversion)
ut_executable(test_reg test_reg.cpp)
target_sources(test_reg PRIVATE test_regtest_lib.cpp)
target_link_libraries(test_reg regtest_lib)
# The JIT cache of the regression runs; empty runs test_reg with its own default.
set(QDB_JIT_CACHE_DIR "" CACHE PATH "Persistent JIT cache directory passed to test_reg by ctest")
set(QDB_REG_ARGS ${CMAKE_SOURCE_DIR}/test/regtest)
if(QDB_JIT_CACHE_DIR)
    list(APPEND QDB_REG_ARGS --jit:cache_dir=${QDB_JIT_CACHE_DIR})
endif()
# The longest test by far: gtest shards that ctest -j runs in parallel.
foreach(shard RANGE 3)
    add_test(NAME test_reg_${shard} COMMAND test_reg ${QDB_REG_ARGS}
        --gtest_output=xml:${CMAKE_CURRENT_BINARY_DIR}/test_reg_${shard}.xml)
    # COST starts the long tests first when ctest has no timing history, as in CI.
    set_tests_properties(test_reg_${shard} PROPERTIES COST 100
        ENVIRONMENT "GTEST_TOTAL_SHARDS=4;GTEST_SHARD_INDEX=${shard}")
endforeach()
# The whole suite over another table: the symmetric join table
# (test_reg_legacy_join), the row layout of the aggregate table from the start
# (test_reg_row_aggregate) and the global aggregate cascade (test_reg_cascade_global;
# the fixtures are too small for auto to choose it). A full pass costs as much as
# test_reg_0..3 together, so they are registered only on request.
option(QDB_TEST_REG_VARIANTS
    "Register the regression suite variants (test_reg_legacy_join, test_reg_row_aggregate, test_reg_cascade_global)" OFF)
if(QDB_TEST_REG_VARIANTS)
    add_test(NAME test_reg_legacy_join COMMAND test_reg ${QDB_REG_ARGS} --join:table=legacy
        --gtest_output=xml:${CMAKE_CURRENT_BINARY_DIR}/test_reg_legacy_join.xml)
    set_tests_properties(test_reg_legacy_join PROPERTIES COST 400 LABELS variant)
    add_test(NAME test_reg_row_aggregate COMMAND test_reg ${QDB_REG_ARGS} --aggregate:table=row
        --gtest_output=xml:${CMAKE_CURRENT_BINARY_DIR}/test_reg_row_aggregate.xml)
    set_tests_properties(test_reg_row_aggregate PROPERTIES COST 400 LABELS variant)
    add_test(NAME test_reg_cascade_global COMMAND test_reg ${QDB_REG_ARGS} --aggregate:cascade_global=true
        --gtest_output=xml:${CMAKE_CURRENT_BINARY_DIR}/test_reg_cascade_global.xml)
    set_tests_properties(test_reg_cascade_global PROPERTIES COST 400 LABELS variant)
endif()
ut(test_equijoin test_equijoin.cpp)
ut(test_outer_join_to_anti test_outer_join_to_anti.cpp)
ut(test_simplify_outer_joins test_simplify_outer_joins.cpp)
ut(test_subquery_scope test_subquery_scope.cpp)
ut(test_const_fold test_const_fold.cpp)
ut(test_cte_reuse test_cte_reuse.cpp)
ut(test_cte_materialize test_cte_materialize.cpp)
ut(test_config test_config.cpp)
ut(test_engine_settings test_engine_settings.cpp)
ut(test_heap test_heap.cpp)
ut(test_sort_row_id test_sort_row_id.cpp)
ut(test_sort test_sort.cpp)
ut(test_sort_plan test_sort_plan.cpp)
ut(test_rowset_ops test_rowset_ops.cpp)
ut(test_sql_binding test_sql_binding.cpp)
ut(test_late_materialization test_late_materialization.cpp)
ut(test_scheduler_connection test_scheduler_connection.cpp)
ut(test_scheduler_core test_scheduler_core.cpp)
ut(test_operator_instrumentation test_operator_instrumentation.cpp)
ut(test_scheduler_graph test_scheduler_graph.cpp)
ut(test_scheduler_queues test_scheduler_queues.cpp)
ut(test_scheduler_runtime_adapter test_scheduler_runtime_adapter.cpp)
ut(test_scheduler_scan_split test_scheduler_scan_split.cpp)
ut(test_exec_plan_builder test_exec_plan_builder.cpp)
ut(test_scheduler test_scheduler.cpp)
ut(test_scheduler_threaded test_scheduler_threaded.cpp)
ut(test_window test_window.cpp)
ut(test_window_plan test_window_plan.cpp)
ut(test_window_exec test_window_exec.cpp)

# JavaScript execution tests (Node.js required), matching externals/qumir.
find_program(QDB_NODE_EXECUTABLE node)
if(QDB_NODE_EXECUTABLE)
    add_test(
        NAME test_plan_export_integration
        COMMAND ${QDB_NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/plan_export/run.mjs
            $<TARGET_FILE:qdb>
            ${CMAKE_SOURCE_DIR}/service/static/browser_runtime.js
            ${CMAKE_SOURCE_DIR}/test/regtest
            ${CMAKE_SOURCE_DIR}/test/plan_export/goldens
            --xml ${CMAKE_CURRENT_BINARY_DIR}/test_plan_export_integration.xml
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    # The browser runner's own handling of expected errors.
    add_test(
        NAME test_reg_lib_js
        COMMAND ${QDB_NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/regtest_lib_self_test.mjs
            --xml ${CMAKE_CURRENT_BINARY_DIR}/test_reg_lib_js.xml
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    # The shared regression suites whose variants list "js".
    add_test(
        NAME test_reg_js
        COMMAND ${QDB_NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_reg.mjs
            $<TARGET_FILE:qdb>
            ${CMAKE_SOURCE_DIR}/service/static/browser_runtime.js
            ${CMAKE_SOURCE_DIR}/test/regtest
            --xml ${CMAKE_CURRENT_BINARY_DIR}/test_reg_js.xml
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    set_tests_properties(test_reg_js PROPERTIES COST 100)
endif()
