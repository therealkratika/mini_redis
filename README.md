# MiniRedis

MiniRedis stores keys in memory and persists mutations to `appendonly.aof` in
the working directory. On startup, it replays the RESP-encoded append-only log
to restore the store. `SET`, `DEL`, and `EXPIRE` mutations are recorded;
expiration is stored as an absolute deadline so downtime counts toward the TTL.