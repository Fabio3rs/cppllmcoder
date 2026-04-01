find_package(SQLite3 REQUIRED)

add_library(sqlite_vec SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/sqlite-vec/sqlite-vec.c
)

target_include_directories(sqlite_vec
    PRIVATE SYSTEM
        ${SQLite3_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/sqlite-vec
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/generated/sqlite-vec/include
)

target_link_libraries(sqlite_vec PRIVATE SQLite::SQLite3)

set_target_properties(sqlite_vec PROPERTIES
    OUTPUT_NAME "vec0"
    PREFIX ""
)

