# LiteHTTPServer

A small HTTP/1.1 static server and reverse proxy written in C.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## Run

```sh
./build/litehttpserver tests/liteserver.conf
```

The configuration path can also be supplied as the first argument. If omitted, `tests/liteserver.conf` is used. Configuration options and limits are documented in [CONF_HELP.md](CONF_HELP.md).

The default configuration serves files from `./www` on `127.0.0.1:8080`. Change the listener address to `0.0.0.0` to accept connections from other interfaces.

## Request handling

The server uses a fixed pthread worker pool and a bounded connection queue. HTTP request parsing is incremental and supports `Content-Length` and `Transfer-Encoding: chunked`, including chunk extensions and trailers. Static responses support GET and HEAD. Reverse-proxy mode forwards requests to one configured upstream.

The server currently closes each client connection after one request. TLS, persistent keep-alive connections, HTTP/2, and multiple upstream balancing are not implemented.

## Test scripts

Run the HTTP parser and static-server edge-case suite with:

```sh
./test.sh
```

Run the reverse-proxy forwarding and response-relay suite with:

```sh
./test_reverse_proxy.sh
```

Both scripts build the project with CMake, use temporary configuration and test files, start their own local test servers, and clean up after themselves. They require Bash, CMake, a C compiler, and Python 3. The scripts do not need external network access.
