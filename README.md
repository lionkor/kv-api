# KV API

A simple persistent key-value store with REST API, written in C++.


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


