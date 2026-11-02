add_library(qumirdb_remote
    actors/compute.cpp
    actors/gateway.cpp
    actors/local_scheduler.cpp
    actors/pinger.cpp
    actors/query.cpp
    actors/scheduler.cpp
    http/router.cpp
)
target_compile_features(qumirdb_remote PUBLIC cxx_std_23)
target_link_libraries(qumirdb_remote PUBLIC
    coroio_actors
    qumirdb
)
