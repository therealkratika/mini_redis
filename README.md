# MiniRedis

MiniRedis is an **educational Redis-inspired implementation** written in
C++17. It is intended to demonstrate how a small TCP data server can be built
from RESP parsing, concurrent client handling, in-memory storage, persistence,
Pub/Sub, and primary–replica streaming. It is not production Redis and does
not aim to provide Redis compatibility, security, or operational guarantees.

## Features and architecture

- **TCP server:** listens on port `6380` by default and handles each accepted
  client on a separate thread. Pass a port to `miniredis` to run another
  instance, for example `./miniredis 6381`.
- **RESP protocol:** commands are sent as RESP arrays of bulk strings, and
  responses use RESP simple strings, errors, integers, bulk strings, and
  arrays. The server accepts RESP command frames, not Redis inline/Telnet
  command syntax.
- **In-memory storage:** string keys and values are kept in a mutex-protected
  hash map. The command handler dispatches requests separately from the
  networking layer.
- **TTL:** `EXPIRE key seconds` assigns a relative expiry; `TTL key` returns
  remaining whole seconds, `-1` for a key without an expiry, or `-2` for a
  missing/expired key. Expired entries are removed lazily when accessed; there
  is no background expiry thread.
- **Append-only persistence:** successful `SET`, `DEL`, and expiration changes
  are recorded as RESP commands in `appendonly.aof` in the server's working
  directory. The log is replayed at startup. Expiry is recorded as an
  absolute deadline, so time while the server is stopped counts toward TTL.
- **Pub/Sub:** connected clients can subscribe to channels and receive RESP
  message frames. Subscriptions are in memory and end when their client
  disconnects.
- **Primary–replica streaming:** a replica can follow a primary with
  `REPLICAOF host port`. The primary forwards subsequent `SET`, `DEL`, and
  `EXPIRE` mutations. A replica persists replicated mutations and rejects
  local writes while connected.
- **CLI and benchmark:** `miniredis-cli` is an interactive local client;
  `miniredis-benchmark` measures SET/GET throughput and average request
  latency.

The replication mechanism is deliberately simple: it does not send an initial
snapshot, resume missed writes, authenticate peers, or reconnect automatically
after a primary disconnect. Pub/Sub is also connection-local and is not
persisted. The implementation omits many Redis commands, data types, protocol
features, and production safeguards.

## Build

Requirements: a C++17 compiler, GNU Make, and POSIX sockets (Linux or macOS).

```sh
make
```

This builds the server, CLI, and benchmark:

```text
./miniredis
./miniredis-cli
./miniredis-benchmark
```

The server listens on `0.0.0.0:6380`. An optional port argument selects a
different listening port:

```sh
./miniredis 6381
```

## Supported commands

Commands are case-insensitive. Keys and values are strings.

| Command | Behavior |
| --- | --- |
| `PING [message]` | Returns `PONG`, or the message as a bulk string. |
| `SET key value` | Stores a value; returns `OK`. |
| `GET key` | Returns the value or a null bulk string if absent. |
| `DEL key [key ...]` | Deletes keys and returns the number removed. |
| `EXPIRE key seconds` | Sets a key's expiry; returns `1` if the key exists, otherwise `0`. |
| `TTL key` | Returns remaining seconds, `-1` for no expiry, or `-2` when absent/expired. |
| `SUBSCRIBE channel [channel ...]` | Subscribes the current connection and returns subscription acknowledgements. |
| `PUBLISH channel message` | Broadcasts a message and returns the number of subscribers. |
| `REPLICAOF host port` | Configures this server to stream from a primary. |

The CLI accepts one command per input line. For `PUBLISH`, the remainder of
the line after the channel is treated as the message and may contain spaces.
Use `QUIT`, `EXIT`, or Ctrl-D to close the CLI connection.

Example CLI session:

```text
$ ./miniredis-cli
Hello from MiniRedis!
mini-redis> SET name Kratika
+OK
mini-redis> GET name
$7
Kratika
mini-redis> EXPIRE name 60
:1
mini-redis> TTL name
:59
```

## RESP and TCP

The server expects RESP array requests. For example, `SET name Kratika` is
encoded as:

```text
*3\r\n
$3\r\nSET\r\n
$4\r\nname\r\n
$7\r\nKratika\r\n
```

TCP is a byte stream, so one request may arrive across multiple reads and
multiple requests may arrive together. MiniRedis buffers partial frames and
processes pipelined commands in order. Malformed or incomplete requests receive
a protocol error when possible and close only that client connection.

## Persistence

By default, the append-only log is `appendonly.aof` in the process working
directory. Keep that file to recover the in-memory key space after restart.
When using Docker Compose, the working directory is `/data`, backed by the
`miniredis-data` named volume.

## Primary–replica example

Start two servers in separate terminals:

```sh
./miniredis 6380
./miniredis 6381
```

Connect the replica to the primary using the CLI (which defaults to port
6380), or send the RESP command directly to port 6381:

```text
REPLICAOF 127.0.0.1 6380
```

To control primary and replica state independently, run each server with a
different working directory so each uses its own AOF file.

## CLI and benchmark

The interactive client connects to `localhost:6380`:

```sh
./miniredis-cli
```

The benchmark performs SET and GET request phases over persistent TCP
connections. It reports requests per second and average request round-trip
latency:

```sh
./miniredis-benchmark --requests 100000 --clients 8
```

Options: `--host HOST`, `--port PORT`, `--requests COUNT`, and
`--clients COUNT`.

## Tests

Unit tests cover storage, command behavior, TTL, persistence recovery, Pub/Sub,
and replicated mutation application. A separate TCP integration suite starts
temporary server processes to test networking, Pub/Sub, error handling, and
primary–replica streaming:

```sh
make test
make test-network
```

## Docker local development

Build and start MiniRedis, publishing port `6380` and persisting the AOF in a
named volume:

```sh
docker compose up --build
```

Connect from the host with `./miniredis-cli` or a RESP-capable client at
`localhost:6380`. Stop the container with Ctrl-C; the named volume retains
`appendonly.aof` across container recreation.

To stop and remove the named data volume as well:

```sh
docker compose down --volumes
```

The Dockerfile uses Alpine build and runtime stages. The image contains only
the compiled server, keeping the runtime small; the CLI and tests remain host
development tools.
