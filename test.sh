#!/usr/bin/env bash
# Test the HTTP parser and static HTTP server.
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"

command -v cmake >/dev/null 2>&1 || { echo "[FAIL] cmake is required" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "[FAIL] python3 is required" >&2; exit 1; }

cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
ctest --test-dir "$BUILD_DIR" --output-on-failure

SERVER="$BUILD_DIR/litehttpserver"
[[ -x "$SERVER" ]] || { echo "[FAIL] server binary not found: $SERVER" >&2; exit 1; }
TMP="$(mktemp -d)"
PID=""
cleanup() {
    if [[ -n "$PID" ]]; then kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; fi
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

mkdir -p "$TMP/www/subdir"
printf 'lite-http-test-file\n' > "$TMP/www/index.html"
printf 'subdirectory-file\n' > "$TMP/www/subdir/file.txt"
printf 'hidden\n' > "$TMP/www/hidden.txt"

python3 - "$SERVER" "$TMP" <<'PY'
import os, socket, subprocess, sys, tempfile, time
server, tmp = sys.argv[1:]
root = os.path.join(tmp, 'www')
s = socket.socket(); s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]; s.close()
conf = os.path.join(tmp, 'server.conf')
with open(conf, 'w') as f:
    f.write(f'''lite-server-host=127.0.0.1
lite-server-port={port}
listen-backlog=64
max-workers=4
max-queue=32
max-headers=16
max-header-size=4096
max-chunk-line-size=128
max-trailer-size=1024
max-body-size=1024
max-request-size=8192
request-timeout=3
reverse-proxy=0
document-root={root}
directory-serve=1
directory-listing=0
allow-logging=0
logging-path={tmp}/server.log
''')
p = subprocess.Popen([server, conf], cwd=tmp, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def ready():
    for _ in range(60):
        if p.poll() is not None: raise RuntimeError('server exited during startup')
        try:
            x=socket.create_connection(('127.0.0.1', port), .1); x.close(); return
        except OSError: time.sleep(.05)
    raise RuntimeError('server did not start')
ready()

def request(data, split=None, pause=.015):
    c=socket.create_connection(('127.0.0.1', port), 2); c.settimeout(3)
    if split:
        pos=0
        for n in split:
            if pos >= len(data): break
            c.sendall(data[pos:pos+n]); pos += n; time.sleep(pause)
        if pos < len(data): c.sendall(data[pos:])
    else: c.sendall(data)
    out=b''
    while True:
        try: b=c.recv(65536)
        except socket.timeout: break
        if not b: break
        out += b
    c.close(); return out

def check(name, raw, status, contains=None, split=None):
    out=request(raw, split)
    first=out.split(b'\r\n',1)[0]
    expected=f'HTTP/1.1 {status} '.encode()
    if not first.startswith(expected):
        raise AssertionError(f'{name}: expected {status}, got {first!r}; response={out[:300]!r}')
    if contains is not None and contains not in out:
        raise AssertionError(f'{name}: response missing {contains!r}')
    print(f'[PASS] {name}: {status}')

try:
    check('basic GET', b'GET / HTTP/1.1\r\nHost: localhost\r\n\r\n', 200, b'lite-http-test-file')
    check('HEAD has headers', b'HEAD / HTTP/1.1\r\nHost: localhost\r\n\r\n', 200)
    check('query string', b'GET /?a=1&b=two HTTP/1.1\r\nHost: localhost\r\n\r\n', 200, b'lite-http-test-file')
    check('additional headers and mixed case', b'GET / HTTP/1.1\r\nhOsT: localhost\r\nUser-Agent: parser-test\r\nX-Empty:\t\r\n\r\n', 200)
    check('fragmented request line and headers', b'GET / HTTP/1.1\r\nHost: localhost\r\nX-Test: fragmented\r\n\r\n', 200, b'lite-http-test-file', [1]*70)
    check('nested file', b'GET /subdir/file.txt HTTP/1.1\r\nHost: localhost\r\n\r\n', 200, b'subdirectory-file')
    check('directory listing disabled', b'GET /subdir/ HTTP/1.1\r\nHost: localhost\r\n\r\n', 403)
    check('method parsed then rejected by static handler', b'POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\ndata', 405)
    check('zero content length', b'POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n', 405)
    check('identical duplicate content lengths', b'POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\nContent-Length: 4\r\n\r\ndata', 405)
    check('chunked with extension and trailer', b'POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n4;name=value\r\ntest\r\n0\r\nX-Checksum: ok\r\n\r\n', 405, split=[1]*150)
    check('chunked empty body', b'POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n', 405)
    check('chunked multiple chunks', b'POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n3\r\none\r\n3\r\ntwo\r\n0\r\n\r\n', 405)
    check('missing Host rejected', b'GET / HTTP/1.1\r\n\r\n', 400)
    check('duplicate Host rejected', b'GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n', 400)
    check('Content-Length and Transfer-Encoding conflict', b'POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\nx', 400)
    check('unsupported transfer coding', b'POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: gzip\r\n\r\n', 400)
    check('invalid chunk size', b'POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\nG\r\nx\r\n0\r\n\r\n', 400)
    check('invalid chunk data delimiter', b'POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nxXX0\r\n\r\n', 400)
    check('malformed header without colon', b'GET / HTTP/1.1\r\nHost: localhost\r\nBroken-Header\r\n\r\n', 400)
    check('bare LF rejected', b'GET / HTTP/1.1\nHost: localhost\r\n\r\n', 400)
    check('HTTP/1.0 rejected', b'GET / HTTP/1.0\r\nHost: localhost\r\n\r\n', 400)
    check('too many headers rejected', b'GET / HTTP/1.1\r\nHost: localhost\r\n' + b''.join(f'X-{i}: a\r\n'.encode() for i in range(20)) + b'\r\n', 400)
    check('oversized body rejected', b'POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2048\r\n\r\n', 413)
    check('conflicting duplicate Content-Length rejected', b'POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nx', 400)
    check('duplicate Transfer-Encoding rejected', b'POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n', 400)
    check('non-numeric Content-Length rejected', b'POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1x\r\n\r\nx', 400)
    check('negative Content-Length rejected', b'POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: -1\r\n\r\n', 400)
    check('chunk data missing CRLF rejected', b'POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nxX\n0\r\n\r\n', 400)
    check('chunk exceeding body limit rejected', b'POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n401\r\n', 413)
    check('oversized header block rejected', b'GET / HTTP/1.1\r\nHost: localhost\r\nX-Large: ' + b'a'*4100 + b'\r\n\r\n', 413)
    check('unsupported Expect rejected', b'POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nExpect: something-else\r\n\r\n', 417)
    # 100-continue must arrive before the request body is sent.
    c=socket.create_connection(('127.0.0.1',port),2); c.settimeout(2)
    c.sendall(b'POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n')
    interim=c.recv(128)
    if b'100 Continue' not in interim: raise AssertionError(f'Expect: 100-continue failed: {interim!r}')
    c.sendall(b'data'); final=b''
    while True:
        b=c.recv(4096)
        if not b: break
        final+=b
    c.close()
    if not final.startswith(b'HTTP/1.1 405 '): raise AssertionError(f'Expect final response invalid: {final[:100]!r}')
    print('[PASS] Expect: 100-continue handshake')
except Exception:
    p.terminate(); p.wait(timeout=3)
    raise
finally:
    p.terminate()
    try: p.wait(timeout=3)
    except subprocess.TimeoutExpired: p.kill(); p.wait()
print('All HTTP server integration tests passed.')
PY
