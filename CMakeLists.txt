cmake_minimum_required(VERSION 3.21)

project(NodeSignalMessenger VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)

find_package(SQLite3 QUIET)
if(NOT SQLite3_FOUND)
    pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
endif()

if(TARGET SQLite::SQLite3)
    set(NS_SQLITE_TARGET SQLite::SQLite3)
else()
    set(NS_SQLITE_TARGET PkgConfig::SQLITE3)
endif()

add_library(nodesignal_comm STATIC
    comm/comm.c
)
target_include_directories(nodesignal_comm
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/comm
)

if(WIN32)
    target_compile_definitions(nodesignal_comm
        PUBLIC
            _CRT_SECURE_NO_WARNINGS
            _WIN32_WINNT=0x0601
    )
    target_link_libraries(nodesignal_comm PUBLIC ws2_32)
endif()

add_library(nodesignal_db STATIC
    database/db.c
)
target_include_directories(nodesignal_db
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/database
)
target_link_libraries(nodesignal_db
    PUBLIC
        nodesignal_comm
        ${NS_SQLITE_TARGET}
)

add_executable(nodesignal_server
    server/server.c
)
target_include_directories(nodesignal_server
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/server
        ${CMAKE_CURRENT_SOURCE_DIR}/comm
        ${CMAKE_CURRENT_SOURCE_DIR}/database
)
target_link_libraries(nodesignal_server
    PRIVATE
        nodesignal_comm
        nodesignal_db
)

add_executable(nodesignal_client
    client/client.c
)
target_include_directories(nodesignal_client
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/client
        ${CMAKE_CURRENT_SOURCE_DIR}/comm
)
target_link_libraries(nodesignal_client
    PRIVATE
        nodesignal_comm
        PkgConfig::GTK4
)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/assets)
configure_file(client/client.ui ${CMAKE_BINARY_DIR}/assets/client.ui COPYONLY)
configure_file(client/style.css ${CMAKE_BINARY_DIR}/assets/style.css COPYONLY)
file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}/assets" NS_ASSET_DIR)

target_compile_definitions(nodesignal_client
    PRIVATE
        NS_ASSET_DIR="${NS_ASSET_DIR}"
)
