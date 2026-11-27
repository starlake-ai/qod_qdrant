# The standalone loadable extension, built by DuckDB's extension build (extension-ci-tools, the extension
# template's layout): src/ against the stand-ins of QoD core's hooks in host/standalone, which come first on the
# include path. Included from the root CMakeLists.txt when build_loadable_extension exists.
set(TARGET_NAME qod_qdrant)
find_package(OpenSSL REQUIRED)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)
project(${TARGET_NAME})
set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(QOD_QDRANT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
get_filename_component(QOD_QDRANT_ROOT "${QOD_QDRANT_ROOT}" ABSOLUTE)

# httplib.h alone in a build-tree directory: on a case insensitive file system the VERSION file next to it
# would shadow the C++ standard header <version>.
configure_file("${QOD_QDRANT_ROOT}/third_party/httplib/httplib.h" "${CMAKE_CURRENT_BINARY_DIR}/httplib_include/httplib.h"
               COPYONLY)

include_directories("${QOD_QDRANT_ROOT}/host/standalone" "${QOD_QDRANT_ROOT}/host" "${QOD_QDRANT_ROOT}/src"
                    "${CMAKE_CURRENT_BINARY_DIR}/httplib_include")
add_definitions(-DCPPHTTPLIB_OPENSSL_SUPPORT -DCPPHTTPLIB_DISABLE_MACOSX_AUTOMATIC_ROOT_CERTIFICATES)

file(GLOB_RECURSE QOD_QDRANT_SOURCES CONFIGURE_DEPENDS "${QOD_QDRANT_ROOT}/src/*.cpp")
set(EXTENSION_SOURCES ${QOD_QDRANT_SOURCES} "${QOD_QDRANT_ROOT}/host/standalone/host_standalone.cpp")

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})
target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
