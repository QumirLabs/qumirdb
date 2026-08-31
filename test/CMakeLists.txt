find_package(PkgConfig REQUIRED)

pkg_check_modules(GTEST gtest REQUIRED)

macro(ut name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} qumirdb ${GTEST_LIBRARIES})
    target_include_directories(${name} PRIVATE ${GTEST_INCLUDE_DIRS})
    target_link_directories(${name} PRIVATE ${GTEST_LIBRARY_DIRS})
    target_compile_options(${name} PRIVATE -Wno-character-conversion)
    add_test(NAME ${name} COMMAND ${name} ${ARGN} --gtest_output=xml:${CMAKE_CURRENT_BINARY_DIR}/${name}.xml)
endmacro()

ut(test_io test_io.cpp)
ut(test_parquet_row_reader test_parquet_row_reader.cpp)
ut(test_stats test_stats.cpp)
ut(test_row_group_predicate test_row_group_predicate.cpp)
ut(test_row_group_pushdown_e2e test_row_group_pushdown_e2e.cpp)
ut(test_cbo test_cbo.cpp)
ut(test_sexp test_sexp.cpp)
ut(test_aggregate test_aggregate.cpp)
ut(test_aggregate_f64 test_aggregate_f64.cpp)
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
ut(test_reg test_reg.cpp ${CMAKE_SOURCE_DIR}/test/regtest)
ut(test_equijoin test_equijoin.cpp)
ut(test_outer_join_to_anti test_outer_join_to_anti.cpp)
ut(test_const_fold test_const_fold.cpp)
ut(test_cte_reuse test_cte_reuse.cpp)
ut(test_cte_materialize test_cte_materialize.cpp)
ut(test_heap test_heap.cpp)
ut(test_sort_row_id test_sort_row_id.cpp)
ut(test_sort test_sort.cpp)
ut(test_sort_plan test_sort_plan.cpp)
ut(test_late_materialization test_late_materialization.cpp)
ut(test_scheduler_connection test_scheduler_connection.cpp)
ut(test_scheduler_core test_scheduler_core.cpp)
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

add_executable(plan_export_native_runner plan_export_native_runner.cpp)
target_link_libraries(plan_export_native_runner PRIVATE qumirdb)
target_compile_features(plan_export_native_runner PRIVATE cxx_std_23)

# JavaScript execution tests (Node.js required), matching externals/qumir.
find_program(QDB_NODE_EXECUTABLE node)
if(QDB_NODE_EXECUTABLE)
    add_test(
        NAME test_plan_export_integration
        COMMAND ${QDB_NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/plan_export/run.mjs
            $<TARGET_FILE:qdb>
            $<TARGET_FILE:plan_export_native_runner>
            ${CMAKE_SOURCE_DIR}/service/static/browser_runtime.js
            ${CMAKE_SOURCE_DIR}/test/plan_export/goldens
            --xml ${CMAKE_CURRENT_BINARY_DIR}/test_plan_export_integration.xml
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()
