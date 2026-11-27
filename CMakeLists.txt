cmake_minimum_required(VERSION 3.20)

# Built by DuckDB's extension build (the extension template, extension_config.cmake): the standalone loadable
# extension, against the stand-ins in host/standalone. Otherwise: the QoD build below, linked into qod-node.
if(COMMAND build_loadable_extension)
  include("${CMAKE_CURRENT_LIST_DIR}/host/standalone/standalone.cmake")
  return()
endif()

project(qod_qdrant CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# libqod_qdrant.a: the private qod_qdrant extension, linked into the public qod-node through
# QOD_EXTRA_EXTENSIONS (scripts/build-node.sh). It is compiled against the public core's native
# headers (QOD_CORE_DIR/native/qod-node/src) and the DuckDB release core finds at DUCKDB_HOME.
option(QOD_QDRANT_TESTS "Build the unit and engine tests (ctest)" OFF)
option(QOD_QDRANT_TESTING "Compile the test-only hooks: transport override and QOD_QDRANT_TEST_REQUEST_LOG" OFF)
option(QOD_QDRANT_ASAN "Build this project's targets and tests (not core's) with AddressSanitizer; a test binary run outside ctest needs ASAN_OPTIONS=detect_container_overflow=0" OFF)

set(QOD_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../quack-on-demand" CACHE PATH "Public core checkout at QOD_CORE_REF")
get_filename_component(QOD_CORE_DIR "${QOD_CORE_DIR}" ABSOLUTE)
set(QOD_NODE_DIR "${QOD_CORE_DIR}/native/qod-node")
if(NOT EXISTS "${QOD_NODE_DIR}/src/protected_scan.hpp")
  message(FATAL_ERROR "QOD_CORE_DIR (${QOD_CORE_DIR}) is not a core checkout with native/qod-node/src/protected_scan.hpp")
endif()

# Core's own project, never built by default: its DuckDB discovery (target qod_node_duckdb, the same
# DUCKDB_HOME rules as the node) and, for the tests, qod_node_core (node hooks, policies, ProtectedScan).
add_subdirectory("${QOD_NODE_DIR}" "${CMAKE_BINARY_DIR}/qod-node-core" EXCLUDE_FROM_ALL)

set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL 3.0 REQUIRED)

# httplib.h is exposed through a build-tree include directory holding only that header: on a case
# insensitive file system the VERSION file next to it would shadow the C++ standard header <version>.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib/httplib.h" "${CMAKE_BINARY_DIR}/httplib_include/httplib.h" COPYONLY)

file(GLOB_RECURSE QOD_QDRANT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(APPEND QOD_QDRANT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/host/qod/host_qod.cpp")
add_library(qod_qdrant STATIC ${QOD_QDRANT_SOURCES})
target_include_directories(qod_qdrant PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/host" "${QOD_NODE_DIR}/src"
  PRIVATE "${CMAKE_BINARY_DIR}/httplib_include")
target_link_libraries(qod_qdrant PUBLIC qod_node_duckdb OpenSSL::SSL OpenSSL::Crypto)
# Trust is only what the transport loads itself (known bundle paths, QOD_QDRANT_CA_FILE), never the macOS
# keychain, which httplib would otherwise link against Security and CoreFoundation for.
target_compile_definitions(qod_qdrant PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT CPPHTTPLIB_DISABLE_MACOSX_AUTOMATIC_ROOT_CERTIFICATES)
if(QOD_QDRANT_TESTING)
  target_compile_definitions(qod_qdrant PUBLIC QOD_QDRANT_TESTING)
endif()
set(QOD_QDRANT_ASAN_FLAGS -fsanitize=address -fno-omit-frame-pointer)
if(QOD_QDRANT_ASAN)
  target_compile_options(qod_qdrant PRIVATE ${QOD_QDRANT_ASAN_FLAGS})
  # PUBLIC: whatever links the instrumented library needs the sanitizer runtime.
  target_link_options(qod_qdrant PUBLIC -fsanitize=address)
endif()

# What the node build must append to its link line (QOD_EXTRA_EXTENSIONS carries no dependencies).
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/qod_qdrant_link_libs.txt"
  CONTENT "${OPENSSL_SSL_LIBRARY};${OPENSSL_CRYPTO_LIBRARY}")

if(QOD_QDRANT_TESTS)
  enable_testing()
  get_filename_component(QOD_DUCKDB_LIB_DIR "${DUCKDB_LIB}" DIRECTORY)
  set(QOD_QDRANT_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  function(qod_qdrant_executable name source)
    add_executable(${name} ${source})
    # qod_qdrant before qod_node_core: the extension references core (RegisterBoundedFunction, ...).
    target_link_libraries(${name} PRIVATE qod_qdrant qod_node_core)
    target_include_directories(${name} PRIVATE "${QOD_QDRANT_TEST_DIR}" "${QOD_NODE_DIR}/tests")
    set_target_properties(${name} PROPERTIES BUILD_RPATH "${QOD_DUCKDB_LIB_DIR}")
    if(QOD_QDRANT_ASAN)
      target_compile_options(${name} PRIVATE ${QOD_QDRANT_ASAN_FLAGS})
    endif()
  endfunction()
  file(GLOB QOD_QDRANT_UNIT_TESTS CONFIGURE_DEPENDS "${QOD_QDRANT_TEST_DIR}/*_test.cpp")
  foreach(source IN LISTS QOD_QDRANT_UNIT_TESTS)
    get_filename_component(name "${source}" NAME_WE)
    qod_qdrant_executable(${name} "${source}")
    add_test(NAME ${name} COMMAND ${name} "${QOD_QDRANT_TEST_DIR}")
  endforeach()
  file(GLOB QOD_QDRANT_PROBES CONFIGURE_DEPENDS "${QOD_QDRANT_TEST_DIR}/*_probe.cpp")
  foreach(source IN LISTS QOD_QDRANT_PROBES)
    get_filename_component(name "${source}" NAME_WE)
    qod_qdrant_executable(${name} "${source}")
  endforeach()
  find_program(BASH bash REQUIRED)
  file(GLOB QOD_QDRANT_SCRIPT_TESTS CONFIGURE_DEPENDS "${QOD_QDRANT_TEST_DIR}/*_test.sh")
  foreach(script IN LISTS QOD_QDRANT_SCRIPT_TESTS)
    get_filename_component(name "${script}" NAME_WE)
    add_test(NAME ${name} COMMAND ${BASH} "${script}" "${QOD_QDRANT_TEST_DIR}" "${CMAKE_BINARY_DIR}")
  endforeach()
  # linked_node_test builds its nodes with scripts/build-node.sh: they use this build's core, DuckDB, OpenSSL
  # and build type, not whatever the environment ctest runs in holds (the test sets QOD_QDRANT_TESTING itself).
  get_filename_component(QOD_QDRANT_DUCKDB_HOME "${DUCKDB_INCLUDE_DIR}" DIRECTORY)
  if(OPENSSL_ROOT_DIR)
    get_filename_component(QOD_QDRANT_OPENSSL_ROOT "${OPENSSL_ROOT_DIR}" ABSOLUTE)
  else()
    get_filename_component(QOD_QDRANT_OPENSSL_ROOT "${OPENSSL_INCLUDE_DIR}" DIRECTORY)
  endif()
  set_tests_properties(linked_node_test PROPERTIES ENVIRONMENT
    "QOD_CORE_DIR=${QOD_CORE_DIR};DUCKDB_HOME=${QOD_QDRANT_DUCKDB_HOME};OPENSSL_ROOT_DIR=${QOD_QDRANT_OPENSSL_ROOT};CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
  if(QOD_QDRANT_ASAN)
    # Core (qod_node_core) and DuckDB are not instrumented: std::vector code they inline is not annotated, so
    # ASan's container-overflow check reports false positives at the boundary. Every other check stays on.
    get_property(QOD_QDRANT_ALL_TESTS DIRECTORY PROPERTY TESTS)
    set_property(TEST ${QOD_QDRANT_ALL_TESTS} APPEND PROPERTY ENVIRONMENT "ASAN_OPTIONS=detect_container_overflow=0")
  endif()
endif()
