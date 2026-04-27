# NodeSignal Messenger

NodeSignal Messenger is a first-iteration multi-client messaging app built around three core C modules:

- `server/` - a `select()`-based socket server that accepts clients, routes messages, and persists chat activity
- `comm/` - the shared protocol and networking layer used by both the server and the client
- `client/` - a GTK4 desktop GUI that connects to the server and updates the UI from a background receive thread

The server stores users and messages in SQLite through the `database/` module.

## Architecture

```text
GTK4 Client UI
    |
Client Controller (C)
    |
comm module  <==== TCP sockets ====>  comm module
                                       |
                                     Server
                                       |
                                    SQLite
```

## Repository Layout

```text
.
|-- CMakeLists.txt
|-- client/
|   |-- client.c
|   |-- client.h
|   |-- client.ui
|   `-- style.css
|-- comm/
|   |-- comm.c
|   `-- comm.h
|-- database/
|   |-- db.c
|   |-- db.h
|   `-- schema.sql
`-- server/
    |-- server.c
    `-- server.h
```

## Current Feature Scope

- username-based join flow
- global chat room
- multiple simultaneous clients handled by one server process
- SQLite persistence for users and messages
- GTK4 desktop GUI with a background receive thread
- shared fixed-header protocol with bounded payload sizes

## Protocol

Each message uses a fixed 16-byte header plus a UTF-8 payload:

- `JOIN` - sent by the client when connecting
- `TEXT` - sent by the client and broadcast by the server
- `LEAVE` - sent by the client on a clean disconnect
- `ACK` - sent by the server after a successful join
- `ERROR` - sent by the server when a request is invalid

The payload limit is `512` bytes for v1.

## Build

Dependencies:

- CMake 3.21+
- GTK4 development package
- SQLite3 development package
- a C compiler
- `pkg-config` for GTK4 discovery

    sudo apt update
    sudo apt install build-essential cmake pkg-config libgtk-4-dev libsqlite3-dev

Generated files in `build/` such as `Makefile` and `CMakeCache.txt` are machine-specific.
If you move the repository to another machine or VM, configure it again in a fresh build directory.

Configure and build:

```sh
cmake -S . -B build
cmake --build build -j
```

If you need to choose a specific compiler, pass it during configuration, for example:

```sh
cmake -S . -B build -DCMAKE_C_COMPILER=gcc
```

The client UI assets are copied into `build/assets` during configuration.

## Package

To create a portable install-style folder, run:

```sh
cmake --install build --prefix dist
```

This produces a layout like:

```text
dist/
|-- bin/
|   |-- nodesignal_client(.exe)
|   |-- nodesignal_server(.exe)
|   |-- assets/
|   |   |-- client.ui
|   |   `-- style.css
|   `-- database/
```

On Windows, the install step also attempts to copy the runtime DLL dependencies needed by the executables into `dist/bin`.

## Run

Start the server:

```sh
./build/nodesignal_server (port#) (databasepath)
./build/nodesignal_server 5555 database/messages.db
```

Start one or more clients:

```sh
./build/nodesignal_client
```

On Windows, replace `./build/...` with the generated `.exe` path.

If you installed into `dist`, run the packaged binaries from `dist/bin`:

```sh
./dist/bin/nodesignal_server
./dist/bin/nodesignal_client
```

The packaged server now defaults to `dist/bin/database/messages.db`, and the packaged client loads `assets/client.ui` and `assets/style.css` relative to its own executable directory.

## Docker

Create a docker image:

```sh
docker build -t cmake-builder .
```

Build using docker:

```sh
docker run --rm   -v "$(pwd):/app"   cmake-builder   bash -c "mkdir -p build && cd build && cmake .. && make"
```
    
