PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# The standalone loadable extension (DuckDB's extension template). The QoD build is scripts/build-node.sh.
EXT_NAME=qod_qdrant
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

include extension-ci-tools/makefiles/duckdb_extension.Makefile
