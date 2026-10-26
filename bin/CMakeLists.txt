find_package(PkgConfig REQUIRED)
pkg_check_modules(READLINE REQUIRED IMPORTED_TARGET readline)
find_package(LLVM REQUIRED CONFIG)

add_executable(qdb cli.cpp plan_export.cpp)
target_compile_features(qdb PUBLIC cxx_std_23)
target_link_libraries(qdb PUBLIC qumirdb PkgConfig::READLINE)
target_include_directories(qdb PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(qdb PRIVATE ${LLVM_DEFINITIONS}
    QDB_VERSION_STRING="${QDB_VERSION_STRING}")

if(QDB_BUILD_DAEMON)
    add_executable(qdbd daemon.cpp)
    target_link_libraries(qdbd PUBLIC qumirdb_remote)
endif()

install(TARGETS qdb
    RUNTIME DESTINATION ${QDB_PRIVATE_BINDIR}
    COMPONENT engine)

# The real binary stays in the versioned tree; the executable path resolves through
# this link, so qdb still finds the oz modules relative to itself.
install(CODE "
    set(_bindir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin\")
    file(MAKE_DIRECTORY \"\${_bindir}\")
    file(CREATE_LINK \"../${QDB_PRIVATE_BINDIR}/qdb\" \"\${_bindir}/qdb\" SYMBOLIC)
" COMPONENT engine)
