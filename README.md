# KV API

A simple persistent (disk-backed) key-value store with REST API, written in C++.

## How to use

Do NOT expose this to the internet without sufficient authentication by a proxy. 
This API is meant as an internal (local network) API and does not protect against attacks of any kind.

**If you MUST host this open to the internet**, use something like nginx to reverse proxy, add TLS (https), 
and add authentication (at the very least [HTTP basic authentication](https://docs.nginx.com/nginx/admin-guide/security-controls/configuring-http-basic-authentication/)).

The key-value store is **append-only**. This means that any new keys, or any updates to old keys, create a new entry
in the kv store on the disk. The key value store will thus grow with every key update. Use the `/merge` endpoint to 
cause a merge of all keys.

### Endpoints

NOTE: KEY must match the regex `[a-zA-Z\d\-_]+`.

- `GET /kv/KEY`: Put a new value for the key supplied after `/kv/`. New value of the key goes in the body.
- `POST /kv/KEY`: Get the value for the key supplied after `/kv/`.
- `GET /help`: A html help page with this information and more.
- `GET /merge`: Causes an immediate merge of the key-value store. Should be ran after adding a lot of keys, or after updating keys.

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

