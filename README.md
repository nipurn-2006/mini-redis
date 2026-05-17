# mini-redis

a redis-like in-memory key-value store written in C++. built this to understand how databases actually work under the hood — not just use them.

## why

i kept using Redis in projects without really knowing what was happening inside. so i built one. turns out it's a TCP server wrapping a hashmap with some extra logic. this repo is that "extra logic" part.

## what it does

connects over TCP on port 6379 (same as real Redis). send it commands, get responses.

```
SET name nipurn     → OK
GET name            → nipurn
EXPIRE name 10      → 1
TTL name            → 9
GET name            → NULL   (after 10s)
DEL name            → 1
EXISTS name         → 0
```

## how to run

```bash
g++ -o server server.cpp
./server
```

then in another terminal:

```bash
nc localhost 6379
```

type commands and hit enter.

## features

**LRU eviction** — store has a max capacity. when it fills up, the least recently used key gets evicted. implemented with a doubly linked list + hashmap so GET, SET, and eviction are all O(1).

**TTL / EXPIRE** — keys can have a timeout. expiry is checked lazily on access using `steady_clock` — same approach real Redis uses. no background threads needed.

**AOF persistence** — every write command is appended to `appendonly.aof`. on restart, the log is replayed to restore state. so your data survives crashes.

**TCP server** — real clients connect over the network. not stdin. had to handle recv buffering properly since TCP doesn't guarantee one recv = one command.

## the interesting bugs

the recv buffer was merging commands — if you sent SET and GET fast enough, the server would read them as one blob. fixed by accumulating data and splitting on newlines instead of assuming each recv call gives exactly one command.

## what i learned

- how socket syscalls actually work (`socket` → `bind` → `listen` → `accept` → `recv/send`)
- why LRU needs both a linked list and a hashmap — one alone doesn't give you O(1)
- what AOF persistence actually means and why Redis flushes to disk on every write
- lazy vs active expiration tradeoffs

## what's missing (known limitations)

- single client at a time (no event loop / multithreading)
- no AUTH or ACL
- no replication
- EXPIRE doesn't persist correctly across restarts (timestamp vs relative seconds problem)

## structure

```
mini-redis/
└── server.cpp    # everything in one file for now
```

## references

- [Redis internals docs](https://redis.io/docs/reference/)
- [Beej's Guide to Network Programming](https://beej.us/guide/bgnet/)
