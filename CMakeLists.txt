# ===================================================================================
# CMakeLists.txt -- Defines the CMake build configuration for NodeSignal Messenger
#     -- Configures the C project and compiler settings
#     -- Locates GTK4 and SQLite dependencies
#     -- Builds the shared communication and database libraries
#     -- Builds the server and client executables
#     -- Copies client UI assets into the build output directory
# ===================================================================================

cmake_minimum_required(VERSION 3.21)

# Project configuration for the NodeSignal Messenger build.
project(NodeSignalMessenger VERSION 0.1.0 LANGUAGES C)

if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

include(GNUInstallDirs)

# Require C17 and disable compiler-specific language extensions.
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Enable strict warning levels for MSVC and non-MSVC compilers.
if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Locate pkg-config and use it to resolve the GTK4 dependency.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)

# Prefer the CMake SQLite target when available, otherwise fall back to pkg-config.
find_package(SQLite3 QUIET)
if(NOT SQLite3_FOUND)
    pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
endif()

if(TARGET SQLite3::SQLite3)
    set(NS_SQLITE_TARGET SQLite3::SQLite3)
elseif(TARGET SQLite::SQLite3)
    set(NS_SQLITE_TARGET SQLite::SQLite3)
else()
    set(NS_SQLITE_TARGET PkgConfig::SQLITE3)
endif()

set(NS_RUNTIME_DEP_DIRS "")
set(NS_RUNTIME_DEP_PRE_EXCLUDES "")
set(NS_RUNTIME_DEP_POST_EXCLUDES "")
if(WIN32)
    # Some Windows dependency chains reference this DLL even when it is not
    # present on the local system, so exclude it before resolution.
    set(NS_RUNTIME_DEP_PRE_EXCLUDES
        "api-ms-"
        "ext-ms-"
        "HvsiFileTrust\\.dll"
    )
    set(NS_RUNTIME_DEP_POST_EXCLUDES
        ".*/[Ss][Yy][Ss][Tt][Ee][Mm]32/.*"
        ".*/[Ss][Yy][Ss][Ww][Oo][Ww]64/.*"
    )

    if(DEFINED ENV{MSYSTEM_PREFIX} AND NOT "$ENV{MSYSTEM_PREFIX}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{MSYSTEM_PREFIX}/bin" NS_MSYSTEM_BIN_DIR)
        list(APPEND NS_RUNTIME_DEP_DIRS "${NS_MSYSTEM_BIN_DIR}")
    endif()

    foreach(directory IN LISTS GTK4_LIBRARY_DIRS SQLITE3_LIBRARY_DIRS)
        get_filename_component(prefix_dir "${directory}" DIRECTORY)
        list(APPEND NS_RUNTIME_DEP_DIRS "${prefix_dir}/bin")
    endforeach()

    list(REMOVE_DUPLICATES NS_RUNTIME_DEP_DIRS)
endif()

# Build the shared communication module used by both client and server.
add_library(nodesignal_comm STATIC
    comm/comm.c
)
target_include_directories(nodesignal_comm
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/comm
)

# Apply Windows-specific communication settings and link against Winsock.
if(WIN32)
    target_compile_definitions(nodesignal_comm
        PUBLIC
            _CRT_SECURE_NO_WARNINGS
            _WIN32_WINNT=0x0601
    )
    target_link_libraries(nodesignal_comm PUBLIC ws2_32)
endif()

# Build the database module and link it with the communication module and SQLite.
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

# Build the server executable and provide access to its private module headers.
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

# Build the GTK client executable and link it with the communication module and GTK4.
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

# Copy the client UI assets into the build directory for in-tree runs and stage packaging placeholders.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/assets)
configure_file(client/client.ui ${CMAKE_BINARY_DIR}/assets/client.ui COPYONLY)
configure_file(client/style.css ${CMAKE_BINARY_DIR}/assets/style.css COPYONLY)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/package-database)
file(WRITE ${CMAKE_BINARY_DIR}/package-database/.keep "")

if(WIN32)
    install(TARGETS nodesignal_server nodesignal_client
        RUNTIME_DEPENDENCY_SET nodesignal_runtime_deps
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    install(RUNTIME_DEPENDENCY_SET nodesignal_runtime_deps
        PRE_EXCLUDE_REGEXES ${NS_RUNTIME_DEP_PRE_EXCLUDES}
        POST_EXCLUDE_REGEXES ${NS_RUNTIME_DEP_POST_EXCLUDES}
        DIRECTORIES ${NS_RUNTIME_DEP_DIRS}
        DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
else()
    install(TARGETS nodesignal_server nodesignal_client
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

install(FILES
    client/client.ui
    client/style.css
    DESTINATION ${CMAKE_INSTALL_BINDIR}/assets
)

install(FILES
    ${CMAKE_BINARY_DIR}/package-database/.keep
    DESTINATION ${CMAKE_INSTALL_BINDIR}/database
)
