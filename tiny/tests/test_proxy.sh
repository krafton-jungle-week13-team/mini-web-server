#!/usr/bin/env bash
# Phase 4 프록시 기능 테스트 (백엔드를 띄우고 프록시를 거쳐 curl로 확인)
# 구성: 프록시 tiny 1개 + 백엔드 tiny 2개 + echo 백엔드(python3) 1개
#
# 실행: tiny 디렉터리에서 make test-proxy (또는 bash tests/test_proxy.sh)
# 포트: PORT 환경변수 (기본 18100). 백엔드는 PORT+1, PORT+3, PORT+5를 쓴다.
#
# echo 백엔드는 받은 경로, body, X-Forwarded-For, Connection 헤더를 그대로 돌려준다.
# tiny 백엔드로는 프록시가 붙인 헤더를 확인할 수 없어서 따로 둔다.

cd "$(dirname "$0")/.." || exit 2

PORT="${PORT:-18100}"
B1=$((PORT + 1))
B2=$((PORT + 3))
ECHO=$((PORT + 5))
BASE="http://localhost:$PORT"
TINY="$PWD/tiny"
TMP=$(mktemp -d)
PIDS=()

pass=0
fail=0

cleanup() {
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null && wait "$pid" 2>/dev/null
    done
    rm -rf "$TMP"
}
trap cleanup EXIT

if [ ! -x ./tiny ] || [ ! -x ./cgi-bin/adder ]; then
    echo "tiny 또는 cgi-bin/adder가 없어요 (make 먼저 실행)" >&2
    exit 2
fi

# ---------- 백엔드 준비 ----------

# tiny 백엔드: 폴더마다 구분용 파일과 CGI 프로그램
for p in "$B1" "$B2"; do
    mkdir -p "$TMP/b$p/api" "$TMP/b$p/cgi-bin"
    echo "tiny-backend-$p" >"$TMP/b$p/api/who.txt"
    cp cgi-bin/adder "$TMP/b$p/cgi-bin/"
done

cat >"$TMP/echo_backend.py" <<'EOF'
import http.server, sys

class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(n)
        out = ("path=%s\nbody=%s\nxff=%s\nconnection=%s\n" % (
            self.path, body.decode(),
            self.headers.get('X-Forwarded-For'),
            ','.join(self.headers.get_all('Connection') or []),
        )).encode()
        self.send_response(200)
        self.send_header('Content-Length', str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def log_message(self, *a):
        pass

http.server.HTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
EOF

cat >"$TMP/routes.conf" <<EOF
/api      no     127.0.0.1:$B1 127.0.0.1:$B2
/calc/    yes    127.0.0.1:$B1 127.0.0.1:$B2
/echo/    yes    127.0.0.1:$ECHO
EOF

# ---------- 서버 실행 ----------

# 백엔드는 각자 폴더에서 실행해야 그 폴더의 파일을 서빙한다
(cd "$TMP/b$B1" && exec "$TINY" "$B1") >"$TMP/b1.log" 2>&1 &
PIDS+=($!)
(cd "$TMP/b$B2" && exec "$TINY" "$B2") >"$TMP/b2.log" 2>&1 &
B2_PID=$!
PIDS+=($B2_PID)
python3 "$TMP/echo_backend.py" "$ECHO" >"$TMP/echo.log" 2>&1 &
PIDS+=($!)
./tiny "$PORT" -c "$TMP/routes.conf" >"$TMP/proxy.log" 2>&1 &
PROXY_PID=$!
PIDS+=($PROXY_PID)

# 네 서버가 모두 응답할 때까지 최대 5초 기다린다
wait_up() {
    for _ in $(seq 1 50); do
        curl -s -o /dev/null "$1" && return 0
        sleep 0.1
    done
    return 1
}
if ! wait_up "http://localhost:$B1/api/who.txt" ||
   ! wait_up "http://localhost:$B2/api/who.txt" ||
   ! wait_up "$BASE/home.html"; then
    echo "서버가 시작되지 않았어요. 프록시 로그:" >&2
    cat "$TMP/proxy.log" >&2
    exit 2
fi
for _ in $(seq 1 50); do
    curl -s -o /dev/null -X POST "http://localhost:$ECHO/" && break
    sleep 0.1
done

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

# expect_body <id> <기대 문자열> <curl 인자...>: 응답 body에 기대 문자열이 있으면 PASS
expect_body() {
    local id="$1" want="$2"
    shift 2
    local got
    got=$(curl -s "$@")
    if printf '%s' "$got" | grep -qF -- "$want"; then
        report "$id" 1
    else
        report "$id" 0 "expected \"$want\", got \"$(printf '%s' "$got" | head -c 120)\""
    fi
}

# expect_status <id> <기대 상태 코드> <curl 인자...>
expect_status() {
    local id="$1" want="$2"
    shift 2
    local got
    got=$(curl -s -o /dev/null -w '%{http_code}' "$@")
    if [ "$got" = "$want" ]; then
        report "$id" 1
    else
        report "$id" 0 "expected $want, got $got"
    fi
}

# ---------- 전달과 라운드로빈 ----------

expect_body "TC-PX-01 GET /api 전달 (1번째 → B1)" "tiny-backend-$B1" "$BASE/api/who.txt"
expect_body "TC-PX-02 라운드로빈 (2번째 → B2)" "tiny-backend-$B2" "$BASE/api/who.txt"
expect_body "TC-PX-03 라운드로빈 (3번째 → B1)" "tiny-backend-$B1" "$BASE/api/who.txt"

# ---------- strip + CGI ----------

# 두 백엔드 모두 adder가 있으므로 어느 쪽으로 가도 같은 결과
expect_body "TC-PX-04 strip + CGI GET" "1 + 2 = 3" "$BASE/calc/cgi-bin/adder?num1=1&num2=2"
expect_body "TC-PX-05 strip + CGI POST (body 전달)" "3 + 4 = 7" \
    -X POST -d 'num1=3&num2=4' "$BASE/calc/cgi-bin/adder"

# ---------- 프록시가 붙이는 헤더 (echo 백엔드) ----------

ECHO_OUT=$(curl -s -X POST -d 'a=1' -H 'Connection: keep-alive' "$BASE/echo/form")
check_echo() {
    local id="$1" want="$2"
    if printf '%s' "$ECHO_OUT" | grep -qxF -- "$want"; then
        report "$id" 1
    else
        report "$id" 0 "expected line \"$want\" in: $(printf '%s' "$ECHO_OUT" | tr '\n' ' ')"
    fi
}
check_echo "TC-PX-06 strip된 경로" "path=/form"
check_echo "TC-PX-07 body 그대로" "body=a=1"
if printf '%s' "$ECHO_OUT" | grep -qxE 'xff=(127\.0\.0\.1|::1|::ffff:127\.0\.0\.1)'; then
    report "TC-PX-08 X-Forwarded-For 추가" 1
else
    report "TC-PX-08 X-Forwarded-For 추가" 0 "expected loopback xff in: $(printf '%s' "$ECHO_OUT" | tr '\n' ' ')"
fi
check_echo "TC-PX-09 hop-by-hop 제거, Connection: close만" "connection=close"

# ---------- 응답 중계, 직접 응답 ----------

expect_status "TC-PX-10 백엔드의 404를 그대로 중계" 404 "$BASE/api/none.txt"
expect_status "TC-PX-11 백엔드의 405를 그대로 중계" 405 -X POST -d 'a=1' "$BASE/api/form"
expect_status "TC-PX-12 규칙 없는 요청은 직접 응답" 200 "$BASE/home.html"
expect_status "TC-PX-13 경계 검사: /apix는 프록시 안 함" 404 "$BASE/apix"

# ---------- 백엔드 장애 ----------

# B2를 끄면 /api 요청 두 번 중 B2 차례 하나는 502, B1 차례 하나는 200
kill "$B2_PID" 2>/dev/null && wait "$B2_PID" 2>/dev/null
codes="$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/who.txt") $(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/who.txt")"
case "$codes" in
    "200 502" | "502 200") report "TC-PX-14 꺼진 백엔드 차례는 502" 1 ;;
    *) report "TC-PX-14 꺼진 백엔드 차례는 502" 0 "expected one 200 and one 502, got $codes" ;;
esac

if kill -0 "$PROXY_PID" 2>/dev/null; then
    report "TC-PX-15 백엔드 장애 후에도 프록시 살아 있음" 1
else
    report "TC-PX-15 백엔드 장애 후에도 프록시 살아 있음" 0 "proxy exited"
fi

echo
echo "proxy: $pass passed, $fail failed"
[ "$fail" -eq 0 ]