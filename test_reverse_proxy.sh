#!/usr/bin/env bash
# Test reverse proxy request forwarding and response relaying.
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
SERVER="$BUILD_DIR/litehttpserver"

command -v cmake >/dev/null 2>&1 || { echo "[FAIL] cmake is required" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "[FAIL] python3 is required" >&2; exit 1; }
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
[[ -x "$SERVER" ]] || { echo "[FAIL] server binary not found: $SERVER" >&2; exit 1; }

TMP="$(mktemp -d)"
PID=""
cleanup() {
    if [[ -n "$PID" ]]; then kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; fi
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

python3 - "$SERVER" "$TMP" <<'PY'
import socket, socketserver, subprocess, sys, threading, time, os
server, tmp = sys.argv[1:]

class Upstream(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(3)
        data=b''
        while b'\r\n\r\n' not in data:
            part=self.request.recv(4096)
            if not part: return
            data += part
        head, rest=data.split(b'\r\n\r\n',1)
        lines=head.split(b'\r\n')
        request_line=lines[0]
        headers={}
        for line in lines[1:]:
            if b':' in line:
                k,v=line.split(b':',1); headers[k.strip().lower()]=v.strip()
        body=bytearray(rest)
        if headers.get(b'transfer-encoding',b'').lower()==b'chunked':
            wire=bytes(body); pos=0; decoded=bytearray()
            while True:
                while b'\r\n' not in wire[pos:]:
                    part=self.request.recv(4096)
                    if not part: raise RuntimeError('upstream got truncated chunk line')
                    wire += part
                end=wire.index(b'\r\n',pos)
                size_line=wire[pos:end].split(b';',1)[0]
                size=int(size_line,16); pos=end+2
                while len(wire)-pos < size+2:
                    part=self.request.recv(4096)
                    if not part: raise RuntimeError('upstream got truncated chunk data')
                    wire += part
                if size==0:
                    pos += 2
                    break
                decoded += wire[pos:pos+size]; pos += size+2
            body=decoded
        else:
            need=int(headers.get(b'content-length',b'0'))
            while len(body)<need:
                part=self.request.recv(4096)
                if not part: raise RuntimeError('upstream got truncated body')
                body += part
            body=body[:need]
        method, target, _=request_line.split(b' ',2)
        marker=headers.get(b'x-proxy-test',b'missing')
        response=method+b'|'+target+b'|'+marker+b'|'+bytes(body)
        wire=b'HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: '+str(len(response)).encode()+b'\r\nConnection: close\r\n\r\n'+response
        self.request.sendall(wire)

class ThreadedServer(socketserver.ThreadingTCPServer):
    allow_reuse_address=True
    daemon_threads=True

up=ThreadedServer(('127.0.0.1',0),Upstream)
up_port=up.server_address[1]
threading.Thread(target=up.serve_forever,daemon=True).start()
s=socket.socket(); s.bind(('127.0.0.1',0)); proxy_port=s.getsockname()[1]; s.close()
conf=os.path.join(tmp,'proxy.conf')
with open(conf,'w') as f:
    f.write(f'''lite-server-host=127.0.0.1
lite-server-port={proxy_port}
listen-backlog=64
max-workers=4
max-queue=32
max-headers=32
max-header-size=8192
max-chunk-line-size=128
max-trailer-size=1024
max-body-size=1048576
max-request-size=2097152
request-timeout=4
reverse-proxy=1
document-root={tmp}
directory-serve=0
directory-listing=0
proxy-host=127.0.0.1
proxy-port={up_port}
allow-logging=0
logging-path={tmp}/proxy.log
''')
p=subprocess.Popen([server,conf],cwd=tmp,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
def ready():
    for _ in range(80):
        if p.poll() is not None: raise RuntimeError('proxy server exited during startup')
        try:
            c=socket.create_connection(('127.0.0.1',proxy_port),.1); c.close(); return
        except OSError: time.sleep(.05)
    raise RuntimeError('proxy server did not start')
ready()

def send(raw, fragments=None):
    c=socket.create_connection(('127.0.0.1',proxy_port),2); c.settimeout(4)
    if fragments:
        pos=0
        for n in fragments:
            if pos>=len(raw): break
            c.sendall(raw[pos:pos+n]); pos+=n; time.sleep(.005)
        if pos<len(raw): c.sendall(raw[pos:])
    else: c.sendall(raw)
    out=b''
    while True:
        b=c.recv(65536)
        if not b: break
        out+=b
    c.close(); return out

def expect(name, raw, wanted, fragments=None):
    out=send(raw,fragments)
    if not out.startswith(b'HTTP/1.1 200 '): raise AssertionError(f'{name}: bad upstream status: {out[:200]!r}')
    body=out.split(b'\r\n\r\n',1)[1]
    if body!=wanted: raise AssertionError(f'{name}: expected {wanted!r}, got {body!r}')
    print(f'[PASS] {name}')

try:
    expect('GET method, target, and custom header',
        b'GET /proxy/path?q=one HTTP/1.1\r\nHost: proxy.local\r\nX-Proxy-Test: preserved\r\n\r\n',
        b'GET|/proxy/path?q=one|preserved|')
    expect('Content-Length body forwarding',
        b'POST /submit HTTP/1.1\r\nHost: proxy.local\r\nX-Proxy-Test: cl\r\nContent-Length: 11\r\nContent-Type: text/plain\r\n\r\nhello world',
        b'POST|/submit|cl|hello world', [1]*100)
    expect('chunked body decoding at upstream',
        b'POST /chunked HTTP/1.1\r\nHost: proxy.local\r\nX-Proxy-Test: chunked\r\nTransfer-Encoding: chunked\r\n\r\n4;ext=yes\r\nWiki\r\n5\r\npedia\r\n0\r\nX-End: yes\r\n\r\n',
        b'POST|/chunked|chunked|Wikipedia', [2]*100)
    expect('binary body forwarding',
        b'POST /binary HTTP/1.1\r\nHost: proxy.local\r\nX-Proxy-Test: binary\r\nContent-Length: 5\r\n\r\na\x00b\xffz',
        b'POST|/binary|binary|a\x00b\xffz')
finally:
    p.terminate()
    try: p.wait(timeout=3)
    except subprocess.TimeoutExpired: p.kill(); p.wait()
    up.shutdown(); up.server_close()
print('All reverse proxy integration tests passed.')
PY
