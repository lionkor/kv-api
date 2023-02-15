# KV API

A simple, fast, persistent (disk-backed) key-value store with REST API, written in C++.

## How to use

Do NOT expose this to the internet without sufficient authentication by a proxy. 
This API is meant as an internal (local network) API and does not protect against attacks of any kind.

**If you MUST host this open to the internet**, use something like nginx to reverse proxy, add TLS (https), 
and add authentication (at the very least [HTTP basic authentication](https://docs.nginx.com/nginx/admin-guide/security-controls/configuring-http-basic-authentication/)).

The key-value store is **append-only**. This means that any new keys, or any updates to old keys, create a new entry
in the kv store on the disk. The key value store will thus grow with every key update. Use the `/merge` endpoint to 
cause a merge of all keys (this will cause outdated values to finally be discarded).

### Endpoints

NOTE: KEY must match the regex `[a-zA-Z\d\-_]+`.

- `GET /kv/KEY`: Get the value for the key supplied after `/kv/`.
- `POST /kv/KEY`: Put a new value for the key supplied after `/kv/`. New value of the key goes in the body.
- `GET /help`: A html help page with this information and more.
- `GET /merge`: Causes an immediate merge of the key-value store. Should be ran after adding a lot of keys, or after updating keys.

## Performance

This library is *not* built for performance. However, on a Ryzen 5 4500U + nvme SSD + Linux machine, the following "benchmark" was achieved:

- Ran on localhost, no external network communication
- Built with `-Og -ggdb`

1. Insertion of 10,000 keys, each with a 12 byte value (via REST API, w/ curl): 23.08s
2. Subsequent `siege` of a random `/kv/` key (182000 hits, 9 seconds), so basically random key read time (via REST API): 20,166 transactions per second

During analysis of various performance tests, the two main bottlenecks are:
- Request speed (you can only run `curl -X POST...` so many times at the same time)
- cpp-httplib's `ThreadPool::enqueue` - since each request starts a new connection (which shouldn't be the case in a real use-case), a new thread task is enqueued. This takes forever.

A lot of performance is left on the table, as currently, a mutex is used to make sure each access to the file happens atomically. This is not strictly needed, depending on the implementation, but for now it's needed.

## Building

To build, run cmake (`bin` will be the output directory, `.` the source directory):

```sh
cmake -S . -B bin
```

This will configure it with all default settings.
Then build with:

```sh
cmake --build bin --parallel
```

Executable `./bin/kv-api` is the program. Simply run it, instructions should be clear from the output.

