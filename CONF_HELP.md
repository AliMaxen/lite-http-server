# LiteHTTPServer configuration

The configuration file uses one `key=value` entry per line. Blank lines and lines beginning with `#` or `;` are ignored. Values are not quoted. Invalid entries stop parsing and report the line number. Defaults are used when the file is missing.

## Listener

| Variable | Default | Description |
|---|---:|---|
| `lite-server-host` | `127.0.0.1` | IPv4 address on which the server listens. Use `0.0.0.0` to listen on all IPv4 interfaces. |
| `lite-server-port` | `8080` | TCP listening port, from 1 to 65535. |
| `listen-backlog` | `128` | Kernel pending-connection backlog passed to `listen()`. |

## Concurrency

| Variable | Default | Description |
|---|---:|---|
| `max-workers` | `8` | Fixed number of worker threads. Each worker handles one client at a time. |
| `max-queue` | `128` | Maximum accepted sockets waiting for a worker. When full, the accept loop waits for queue space; the kernel backlog may fill as well. |

## HTTP limits and timeouts

| Variable | Default | Description |
|---|---:|---|
| `max-headers` | `64` | Maximum number of request headers. Hard implementation ceiling: 128. |
| `max-header-size` | `16384` | Maximum total request-header bytes, including request line and delimiters. |
| `max-chunk-line-size` | `128` | Maximum chunk-size line length, including any chunk extensions. |
| `max-trailer-size` | `65536` | Maximum combined chunk-trailer bytes, including CRLF delimiters. |
| `max-body-size` | `16777216` | Maximum decoded request body size in bytes (1 to 1073741824), for both Content-Length and chunked requests. |
| `max-request-size` | `0` | Maximum complete request size on the wire, including headers and chunk framing. Zero derives a limit from the other size limits. Explicit range: 1024 to 2147483648 bytes. |
| `request-timeout` | `15` | Receive and send socket timeout in seconds. |
| `recv-buffer-size` | `0` | Optional kernel receive-buffer size in bytes. Zero leaves the OS default. |
| `send-buffer-size` | `0` | Optional kernel send-buffer size in bytes. Zero leaves the OS default. |

The parser supports HTTP/1.1 request lines, `Content-Length`, and `Transfer-Encoding: chunked`. It rejects unsupported transfer codings and requests containing both Content-Length and Transfer-Encoding. Chunk extensions and trailers are accepted. The body exposed to handlers is decoded (chunk framing is removed).

## Server and reverse proxy modes

| Variable | Default | Description |
|---|---:|---|
| `reverse-proxy` | `0` | `0` serves local files; `1` forwards the complete received HTTP request to the configured upstream and relays its response. |
| `document-root` | `./www` | Filesystem root used when reverse proxy mode is disabled. |
| `directory-serve` | `1` | `0` denies access to directory paths; `1` allows directory paths and serves `index.html` when present. |
| `directory-listing` | `0` | `1` generates a directory index when no `index.html` exists; `0` returns 403 instead. Requires `directory-serve=1`. |
| `proxy-host` | `127.0.0.1` | Upstream hostname or IP address used when reverse proxy mode is enabled. |
| `proxy-port` | `8081` | Upstream TCP port. |

Static mode currently supports GET and HEAD. Paths are constrained to the resolved document root; parent traversal and paths resolving outside the root are rejected. Directory access is enabled by default so an `index.html` can be served; automatic directory listings are disabled by default.

## Logging

| Variable | Default | Description |
|---|---:|---|
| `allow-logging` | `1` | `1` enables logging; `0` disables it. |
| `logging-path` | `liteserver.log` | Append-only log file path. |

Log entries include a timestamp, module, severity, and message. Logging is serialized between worker threads.

## Example

```ini
lite-server-host=0.0.0.0
lite-server-port=8080
max-workers=16
max-queue=256
max-headers=64
max-header-size=16384
max-chunk-line-size=128
max-trailer-size=65536
max-body-size=33554432
max-request-size=0
request-timeout=20
reverse-proxy=0
document-root=./www
directory-serve=1
directory-listing=0
allow-logging=1
logging-path=./liteserver.log
```

Run with `./litehttpserver tests/liteserver.conf`. If no path is supplied, the program reads `tests/liteserver.conf` from the project working directory.
