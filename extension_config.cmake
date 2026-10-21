# This file is included by DuckDB's build system. It specifies which extension to load

duckdb_extension_load(lindel
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
