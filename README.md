# KV API

A simple persistent key-value store with REST API, written in C++.

## How to use

To STORE a value in this key-value store, POST to the endpoint `/kv/my-key` with the body of the request as the value.
For example, `curl -X POST http://HOST:PORT/kv/my-key-2 --data "Hello world!"` will cause the key-value pair `my-key-2=Hello world!` to be stored.
To LOAD / GET a value, use a GET request to the same endpoint. For example, to get the value of key `my-key-2`, GET the `/kv/my-key-2` endpoint.

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

