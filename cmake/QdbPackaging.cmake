set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "qumirdb")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Alexey Ozeritskiy <aozeritsky@gmail.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/resetius/qumirdb")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Analytical query engine over Parquet")
set(CPACK_STRIP_FILES ON)

set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# Yields 0.1.0-42; the hyphen is the Debian revision separator.
set(CPACK_DEBIAN_PACKAGE_RELEASE "${QDB_BUILD_NUMBER}")

set(CPACK_COMPONENTS_ALL engine)

set(CPACK_DEBIAN_ENGINE_PACKAGE_NAME "qumirdb")
set(CPACK_DEBIAN_ENGINE_PACKAGE_SECTION "database")
# External Rust modules are compiled through the local rustc toolchain.
set(CPACK_DEBIAN_ENGINE_PACKAGE_SUGGESTS "rustc")
set(CPACK_COMPONENT_ENGINE_DESCRIPTION
    "QumirDB query engine and CLI.\n Runs SQL over Parquet datasets with JIT-compiled kernels:\n the qdb command line tool and the plan exporter.")

if(QDB_BUILD_SERVICE)
    list(APPEND CPACK_COMPONENTS_ALL service)
    set(CPACK_DEBIAN_SERVICE_PACKAGE_NAME "qumirdb-service")
    set(CPACK_DEBIAN_SERVICE_PACKAGE_SECTION "web")
    # The server only spawns the exporter as a subprocess, so the upstream version is
    # enough; pinning the build number would forbid rebuilds of the same release.
    set(CPACK_DEBIAN_SERVICE_PACKAGE_DEPENDS "qumirdb (>= ${PROJECT_VERSION}), adduser, python3, ca-certificates, cron")
    # Preserve administrator changes to the hourly GitHub statistics job.
    set(CPACK_DEBIAN_SERVICE_PACKAGE_CONTROL_EXTRA
        "${CMAKE_SOURCE_DIR}/service/postinst;${CMAKE_SOURCE_DIR}/service/conffiles")
    set(CPACK_COMPONENT_SERVICE_DESCRIPTION
        "QumirDB workbench web service.\n HTTP server backing the browser workbench: SQL editor,\n plan viewer and dataset management.")
endif()

if(QDB_BUILD_DAEMON)
    list(APPEND CPACK_COMPONENTS_ALL daemon)
    set(CPACK_DEBIAN_DAEMON_PACKAGE_NAME "qumirdb-daemon")
    set(CPACK_DEBIAN_DAEMON_PACKAGE_SECTION "database")
    # qdbd and the oz modules it compiles come from the same build; both packages
    # are released together, so a rebuild bumps both.
    set(CPACK_DEBIAN_DAEMON_PACKAGE_DEPENDS "qumirdb (= ${CPACK_PACKAGE_VERSION}-${CPACK_DEBIAN_PACKAGE_RELEASE}), adduser")
    set(CPACK_DEBIAN_DAEMON_PACKAGE_CONTROL_EXTRA
        "${CMAKE_SOURCE_DIR}/qdbd/package/postinst;${CMAKE_SOURCE_DIR}/qdbd/package/conffiles")
    set(CPACK_COMPONENT_DAEMON_DESCRIPTION
        "QumirDB cluster node daemon.\n qdbd runs a distributed-execution node; the systemd unit is\n installed disabled and reads its arguments from /etc/default/qumirdb-daemon.")
endif()

include(CPack)
