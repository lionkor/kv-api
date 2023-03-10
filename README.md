# KV API

[![Linux](https://github.com/lionkor/kv-api/actions/workflows/cmake-linux.yml/badge.svg)](https://github.com/lionkor/kv-api/actions/workflows/cmake-linux.yml)
[![Windows](https://github.com/lionkor/kv-api/actions/workflows/cmake-windows.yml/badge.svg)](https://github.com/lionkor/kv-api/actions/workflows/cmake-windows.yml)

A simple, fast, persistent (disk-backed) key-value store with REST API, written in C++.

Since v2.0.0, the MIME type of the data is stored, too.

## How to use

Do NOT expose this to the internet without sufficient authentication by a proxy. 
This API is meant as an internal (local network) API and does not protect against attacks of any kind.

**If you MUST host this open to the internet**, use something like nginx to reverse proxy, add TLS (https), 
and add authentication (at the very least [HTTP basic authentication](https://docs.nginx.com/nginx/admin-guide/security-controls/configuring-http-basic-authentication/)).

The key-value store is **append-only**. This means that any new keys, or any updates to old keys, create a new entry
in the kv store on the disk. The key value store will thus grow with every key update. Use the `/merge` endpoint to 
cause a merge of all keys (this will cause outdated values to finally be discarded).

### Endpoints

NOTE: KEY must match the regex `.+` (before version v1.1.0 it was `[a-zA-Z\d\-_]+`). For example, `my-key-1`, `this/looks/like/a/path` and anything else matching `.+` will work. Please be aware that e.g. `/../` is special and will be resolved.

- `GET /kv/KEY`: Get the value for the key supplied after `/kv/`.
- `POST /kv/KEY`: Put a new value for the key supplied after `/kv/`. New value of the key goes in the body.
- `GET /help`: A html help page with this information and more.
- `GET /merge`: Causes an immediate merge of the key-value store. Should be ran after adding a lot of keys, or after updating keys.

### Example Use

```sh
# storing some values
$ curl localhost:8080/kv/name --data "Lion"
OK
$ curl localhost:8080/kv/age --data 24
OK
$ curl localhost:8080/kv/language --data "C++"
OK

# loading some values
$ curl localhost:8080/kv/name
Lion
$ curl localhost:8080/kv/age
24
$ curl localhost:8080/kv/language
C++
```

You can, of couse, also POST and GET binary data, such as files (any data up to 4GB), or json, or whatever you like. It will always be returned as the MIME type you stored it with (or `application/octet-stream`, if `Content-Type` was not supplied).

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

## Troubleshooting

Any known issues are on GitHub under [issues](https://github.com/lionkor/kv-api/issues). When opening an issue, supply the version number and commit. For example, when you run `kv-api`, the first line is something like `KV API v1.1.0-100e648`.

