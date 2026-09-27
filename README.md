# MiniRedis

MiniRedis stores keys in memory and persists mutations to `appendonly.aof` in
the working directory. On startup, it replays the RESP-encoded append-only log
to restore the store. `SET`, `DEL`, and `EXPIRE` mutations are recorded;
expiration is stored as an absolute deadline so downtime counts toward the TTL.

Pub/Sub is available with `SUBSCRIBE channel` and `PUBLISH channel message`.
Messages are sent to all connected subscribers using RESP message frames.

For educational primary-replica replication, start the primary with
`./miniredis 6380` and the replica with `./miniredis 6381`, then send
`REPLICAOF 127.0.0.1 6380` to the replica. The replica applies and persists
subsequent `SET`, `DEL`, and `EXPIRE` mutations from the primary, and rejects
local writes while connected. This simple streaming setup does not perform an
initial snapshot or automatically reconnect after the primary disconnects.

Build both programs with `make`. Run `./miniredis-cli` to connect to
`localhost:6380` and enter RESP commands interactively. `PUBLISH` accepts a
message containing spaces. The client supports `SET`, `GET`, `DEL`, `EXPIRE`,
`TTL`, `SUBSCRIBE`, and `PUBLISH`; use `QUIT` or Ctrl-D to close the client.

Run storage, command, TTL, persistence, Pub/Sub, and replication unit coverage
with `make test`. Run the separate TCP integration suite with
`make test-network`; it starts temporary primary and replica servers on
available local ports. Malformed or incomplete RESP requests receive a
protocol error and close only that client connection. Commands with invalid
arguments and persistence write failures return RESP error replies while the
server continues serving other clients.