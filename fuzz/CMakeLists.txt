# libFuzzer targets of the query pipeline, cheapest stage first. Each links
# libFuzzer's main; the sanitizers come from QDB_FUZZ_SANITIZERS or CFLAGS.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(QDB_FUZZ_DATA ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_data.py)
file(GLOB_RECURSE QDB_FUZZ_FIXTURES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/test/regtest/cases/*.json
    ${CMAKE_CURRENT_SOURCE_DIR}/tables/*.json)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fuzz_tables.inc
    COMMAND Python3::Interpreter ${QDB_FUZZ_DATA} tables ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/fuzz_tables.inc
    DEPENDS ${QDB_FUZZ_DATA} ${QDB_FUZZ_FIXTURES}
    COMMENT "Generating the fuzz table schemas")

# The seed corpus: one file per distinct query of the SQL suites.
file(GLOB_RECURSE QDB_FUZZ_QUERIES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/test/regtest/cases/*.sql
    ${CMAKE_SOURCE_DIR}/benchmark/*.sql)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/corpus.stamp
    COMMAND Python3::Interpreter ${QDB_FUZZ_DATA} corpus ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/corpus
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/corpus.stamp
    DEPENDS ${QDB_FUZZ_DATA} ${QDB_FUZZ_QUERIES}
    COMMENT "Generating the fuzz seed corpus")
add_custom_target(qdb_fuzz_corpus ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/corpus.stamp)

add_library(qdb_fuzz_pipeline STATIC
    pipeline.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/fuzz_tables.inc)
target_include_directories(qdb_fuzz_pipeline PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(qdb_fuzz_pipeline PUBLIC qumirdb)

function(qdb_fuzzer name)
    add_executable(${name} ${ARGN})
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
endfunction()

qdb_fuzzer(qdb_fuzz_parse parse_fuzzer.cpp)
target_link_libraries(qdb_fuzz_parse PRIVATE qumirdb)
qdb_fuzzer(qdb_fuzz_plan plan_fuzzer.cpp)
target_link_libraries(qdb_fuzz_plan PRIVATE qdb_fuzz_pipeline)
qdb_fuzzer(qdb_fuzz_vm vm_fuzzer.cpp)
target_link_libraries(qdb_fuzz_vm PRIVATE qdb_fuzz_pipeline)
