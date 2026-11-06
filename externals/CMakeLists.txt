# EXCLUDE_FROM_ALL drops the submodules' own install rules; their targets are still
# built because qumirdb links them.
add_subdirectory(qumir EXCLUDE_FROM_ALL)
if(QDB_BUILD_SERVICE OR QDB_BUILD_DAEMON)
    add_subdirectory(coroio EXCLUDE_FROM_ALL)
endif()
