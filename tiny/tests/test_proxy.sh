#!/usr/bin/env bash
# Phase 5 리버스 프록시 테스트 (docs/design/phase5/reverse-proxy.md)
#
# 실행: tiny 디렉터리에서 make test-proxy (또는 bash tests/test_proxy.sh)
# 포트: PORT 환경변수(기본 18200)부터 18206까지 써요.
#   PORT    프록시 (tiny -c)
#   PORT+1  백엔드 A (tiny, home.html = "Backend A")
#   PORT+2  백엔드 B (tiny, home.html = "Backend B")
#   PORT+3  아무것도 없음 (죽은 백엔드)
#   PORT+4  echo 백엔드 (받은 요청 원문을 돌려줌)
#   PORT+5  slow 백엔드 (응답하지 않음)
#   PORT+6  empty 백엔드 (응답 없이 닫음)

cd "$(dirname "$0")/.." || exit 2
ROOT=$(pwd)

PORT="${PORT:-18200}"
PA=$((PORT + 1)); PB=$((PORT + 2)); PDEAD=$((PORT + 3))
PECHO=$((PORT + 4)); PSLOW=$((PORT + 5)); PEMPTY=$((PORT + 6))
BASE="http://localhost:$PORT"
TMP=$(mktemp -d)
PIDS=()

pass=0
fail=0

cleanup() {
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

if [ ! -x ./tiny ] || [ ! -x ./cgi-bin/adder ]; then
    echo "tiny 또는 cgi-bin/adder가 없어요 (make 먼저 실행)" >&2
    exit 2
fi

report() {
    local id="$1" ok="$2" why="$3"
    if [ "$ok" = 1 ]; then
        echo "[PASS] $id"
        pass=$((pass + 1))
    else
        echo "[FAIL] $id: $why"
        fail=$((fail + 1))
    fi
}

# wait_port <port>: 포트가 연결을 받을 때까지 최대 5초 기다린다
wait_port() {
    for _ in $(seq 1 50); do
        python3 -c 'import socket,sys; socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.2).close()' "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    echo "포트 $1이 열리지 않았어요" >&2
    exit 2
}

# ---------- 백엔드 준비 ----------
for name in A B; do
    d="$TMP/backend$name"
    mkdir -p "$d/cgi-bin"
    printf '<html><body>Backend %s</body></html>\n' "$name" >"$d/home.html"
    cp godzilla.gif "$d/"
    cp cgi-bin/adder "$d/cgi-bin/"
done
(cd "$TMP/backendA" && exec "$ROOT/tiny" "$PA" >"$TMP/a.log" 2>&1) & PIDS+=($!)
(cd "$TMP/backendB" && exec "$ROOT/tiny" "$PB" >"$TMP/b.log" 2>&1) & PIDS+=($!)
python3 tests/fake_backend.py echo "$PECHO" & PIDS+=($!)
python3 tests/fake_backend.py slow "$PSLOW" & PIDS+=($!)
python3 tests/fake_backend.py empty "$PEMPTY" & PIDS+=($!)

cat >"$TMP/routes.conf" <<EOF
# 테스트용 라우팅 규칙
/a      yes  127.0.0.1:$PA 127.0.0.1:$PB
/mixed  yes  127.0.0.1:$PA 127.0.0.1:$PDEAD
/echo   no   127.0.0.1:$PECHO
/dead   no   127.0.0.1:$PDEAD
/slow   no   127.0.0.1:$PSLOW
/empty  no   127.0.0.1:$PEMPTY
EOF

./tiny "$PORT" -c "$TMP/routes.conf" >"$TMP/proxy.log" 2>&1 & PROXY_PID=$!; PIDS+=($PROXY_PID)
for p in "$PA" "$PB" "$PECHO" "$PSLOW" "$PEMPTY" "$PORT"; do wait_port "$p"; done

CURL=(curl -s --max-time 10 -H 'Expect:' -o "$TMP/body" -w '%{http_code}')

# backend_of: 본문에서 "Backend A" / "Backend B"를 꺼낸다
backend_of() { grep -o 'Backend [AB]' "$TMP/body" | head -n 1; }

# TC-PX-01: 백엔드 두 개에 번갈아 보낸다 (round-robin)
seq_out=""
for k in 1 2 3 4; do
    code=$("${CURL[@]}" "$BASE/a/home.html")
    seq_out="$seq_out$code:$(backend_of),"
done
IFS=, read -r r1 r2 r3 r4 _ <<<"$seq_out"
if [[ "$r1" == 200:Backend* && "$r2" == 200:Backend* && "$r1" != "$r2" && "$r1" == "$r3" && "$r2" == "$r4" ]]; then
    report TC-PX-01 1
else
    report TC-PX-01 0 "응답 순서: $seq_out"
fi

# TC-PX-02: 규칙에 맞지 않는 경로는 프록시가 직접 응답한다
code=$("${CURL[@]}" "$BASE/home.html")
if [ "$code" = 200 ] && cmp -s "$TMP/body" home.html; then
    report TC-PX-02 1
else
    report TC-PX-02 0 "status $code, 본문이 tiny/home.html과 다름"
fi

# TC-PX-03: strip=yes + POST body 전달 (/a/cgi-bin/adder → 백엔드의 /cgi-bin/adder)
code=$("${CURL[@]}" -d 'num1=3&num2=16' "$BASE/a/cgi-bin/adder")
if [ "$code" = 200 ] && grep -qF '3 + 16 = 19' "$TMP/body"; then
    report TC-PX-03 1
else
    report TC-PX-03 0 "status $code, 본문에 '3 + 16 = 19'가 없음"
fi

# TC-PX-04: 쿼리 문자열 전달
code=$("${CURL[@]}" "$BASE/a/cgi-bin/adder?num1=1&num2=2")
if [ "$code" = 200 ] && grep -qF '1 + 2 = 3' "$TMP/body"; then
    report TC-PX-04 1
else
    report TC-PX-04 0 "status $code, 본문에 '1 + 2 = 3'이 없음"
fi

# TC-PX-05: 바이너리 응답을 바이트 그대로 전달
code=$("${CURL[@]}" "$BASE/a/godzilla.gif")
if [ "$code" = 200 ] && cmp -s "$TMP/body" godzilla.gif; then
    report TC-PX-05 1
else
    report TC-PX-05 0 "status $code, 본문이 godzilla.gif와 다름"
fi

# TC-PX-06: 죽은 백엔드 → 502
code=$("${CURL[@]}" "$BASE/dead/x")
[ "$code" = 502 ] && report TC-PX-06 1 || report TC-PX-06 0 "status $code (expected 502)"

# TC-PX-07: 백엔드 하나만 죽음 → 두 번 요청하면 200 한 번, 502 한 번
c1=$("${CURL[@]}" "$BASE/mixed/home.html")
c2=$("${CURL[@]}" "$BASE/mixed/home.html")
pair=$(printf '%s\n%s\n' "$c1" "$c2" | sort | tr '\n' ' ')
[ "$pair" = "200 502 " ] && report TC-PX-07 1 || report TC-PX-07 0 "status $c1, $c2 (expected 200과 502)"

# TC-PX-08: 응답하지 않는 백엔드는 시간 제한 없이 기다린다 (504 없음).
# 그 요청이 worker 하나를 붙잡고 있어도 다른 요청은 남은 worker가 처리한다.
# 붙잡힌 worker는 테스트가 끝나 slow 백엔드가 종료될 때 풀려난다.
curl -s -o /dev/null --max-time 3 -w '%{http_code}' "$BASE/slow/x" >"$TMP/slow_code" &
slow_pid=$!
sleep 0.5
code=$("${CURL[@]}" "$BASE/a/home.html")
wait "$slow_pid"
slow_code=$(cat "$TMP/slow_code")
if [ "$slow_code" = 000 ] && [ "$code" = 200 ]; then
    report TC-PX-08 1
else
    report TC-PX-08 0 "slow 응답 $slow_code (expected 3초 동안 응답 없음 000), 다른 요청 $code (expected 200)"
fi

# TC-PX-09: 응답 없이 연결을 닫는 백엔드 → 502
code=$("${CURL[@]}" "$BASE/empty/x")
[ "$code" = 502 ] && report TC-PX-09 1 || report TC-PX-09 0 "status $code (expected 502)"

# TC-PX-10: 백엔드로 가는 요청 확인 (echo 백엔드가 받은 원문을 돌려줌)
code=$("${CURL[@]}" -H 'Connection: keep-alive' -H 'Keep-Alive: 300' -H 'X-Custom: hi' \
    -H 'X-Forwarded-For: 10.0.0.1' "$BASE/echo/path?x=1")
req=$(tr -d '\r' <"$TMP/body")
why=""
[ "$code" = 200 ] || why="status $code"
[ -z "$why" ] && [ "$(head -n 1 <<<"$req")" != "GET /echo/path?x=1 HTTP/1.0" ] && why="요청줄: $(head -n 1 <<<"$req")"
[ -z "$why" ] && ! grep -qx 'X-Custom: hi' <<<"$req" && why="X-Custom이 전달되지 않음"
[ -z "$why" ] && ! grep -qx "Host: localhost:$PORT" <<<"$req" && why="Host가 그대로 전달되지 않음"
[ -z "$why" ] && ! grep -qx 'X-Forwarded-For: 10.0.0.1, 127.0.0.1' <<<"$req" && why="X-Forwarded-For: $(grep -i '^X-Forwarded-For' <<<"$req")"
[ -z "$why" ] && grep -qi '^Keep-Alive:' <<<"$req" && why="Keep-Alive 헤더가 전달됨"
[ -z "$why" ] && [ "$(grep -ci '^Connection:' <<<"$req")" != 1 ] && why="Connection 헤더 개수: $(grep -ci '^Connection:' <<<"$req")"
[ -z "$why" ] && ! grep -qx 'Connection: close' <<<"$req" && why="Connection: close가 없음"
[ -z "$why" ] && report TC-PX-10 1 || report TC-PX-10 0 "$why"

# TC-PX-11: Host 없는 HTTP/1.0 요청 → 백엔드 주소로 Host를 붙인다
resp=$(printf 'GET /echo/h HTTP/1.0\r\n\r\n' | python3 -c '
import socket, sys
s = socket.create_connection(("localhost", int(sys.argv[1])), timeout=5)
s.sendall(sys.stdin.buffer.read())
out = b""
while True:
    c = s.recv(65536)
    if not c: break
    out += c
sys.stdout.buffer.write(out)
' "$PORT" | tr -d '\r')
if grep -qx "Host: 127.0.0.1:$PECHO" <<<"$resp"; then
    report TC-PX-11 1
else
    report TC-PX-11 0 "백엔드가 받은 Host: $(grep -i '^Host' <<<"$resp")"
fi

# TC-PX-12: HTTP/1.1이어도 프록시 응답 뒤에는 클라이언트 연결을 닫는다
closed=$(python3 -c '
import socket, sys
s = socket.create_connection(("localhost", int(sys.argv[1])), timeout=5)
s.sendall(b"GET /a/home.html HTTP/1.1\r\nHost: localhost\r\n\r\n")
out = b""
try:
    while True:
        c = s.recv(65536)
        if not c:
            print("closed" if b"Backend" in out else "no-body"); break
        out += c
except socket.timeout:
    print("open")
' "$PORT")
[ "$closed" = closed ] && report TC-PX-12 1 || report TC-PX-12 0 "결과: $closed"

# TC-PX-13: 규칙에 맞는 POST도 Content-Length가 없으면 411
code=$(printf 'POST /a/cgi-bin/adder HTTP/1.0\r\nHost: localhost\r\n\r\n' | python3 -c '
import socket, sys
s = socket.create_connection(("localhost", int(sys.argv[1])), timeout=5)
s.sendall(sys.stdin.buffer.read()); s.shutdown(socket.SHUT_WR)
print(s.recv(100).split(b" ")[1].decode())
' "$PORT")
[ "$code" = 411 ] && report TC-PX-13 1 || report TC-PX-13 0 "status $code (expected 411)"

# TC-PX-14: 프록시로 CGI 요청 20개를 동시에 → 모두 자기 결과
cc_pids=()
for k in $(seq 1 20); do
    curl -s --max-time 10 -o "$TMP/px_body_$k" -w '%{http_code}' \
        "$BASE/a/cgi-bin/adder?num1=$k&num2=100" >"$TMP/px_code_$k" &
    cc_pids+=($!)
done
wait "${cc_pids[@]}"
cc_ok=1; cc_why=""
for k in $(seq 1 20); do
    c=$(cat "$TMP/px_code_$k")
    if [ "$c" != 200 ]; then
        cc_ok=0; cc_why="요청 $k: status $c"; break
    elif ! grep -qF "$k + 100 = $((k + 100))" "$TMP/px_body_$k"; then
        cc_ok=0; cc_why="요청 $k: 결과가 다름"; break
    fi
done
report TC-PX-14 "$cc_ok" "$cc_why"

# TC-PX-15: 잘못된 설정 파일 → 서버가 시작하지 않고 이유를 알려 준다
printf '/x maybe 127.0.0.1:1\n' >"$TMP/bad.conf"
out=$(timeout 3 ./tiny $((PORT + 9)) -c "$TMP/bad.conf" 2>&1); rc=$?
if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ] && [[ "$out" == *"routes:1"* ]]; then
    report TC-PX-15 1
else
    report TC-PX-15 0 "rc=$rc, 출력: $out"
fi

# TC-PX-16: 설정 파일이 없으면 서버가 시작하지 않는다
out=$(timeout 3 ./tiny $((PORT + 9)) -c "$TMP/nope.conf" 2>&1); rc=$?
[ "$rc" -ne 0 ] && [ "$rc" -ne 124 ] && report TC-PX-16 1 || report TC-PX-16 0 "rc=$rc, 출력: $out"

if ! kill -0 "$PROXY_PID" 2>/dev/null; then
    echo "[FAIL] 프록시 서버가 테스트 중 종료됨" >&2
    fail=$((fail + 1))
fi

echo
echo "proxy: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
