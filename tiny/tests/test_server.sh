#!/usr/bin/env bash
# Phase 3 서버 기능 테스트 (서버를 띄우고 curl / 요청 원문으로 확인)
# 케이스: docs/test/phase3/test-cases.md 3장 (TC-FN)
#
# 실행: tiny 디렉터리에서 make test-server (또는 bash tests/test_server.sh)
# 포트: PORT 환경변수 (기본 18000, 8000에 띄운 개발용 서버와 겹치지 않게)
#
# 요청 원문 전송은 문서의 `nc -N` 대신 python3 소켓을 써요.
# macOS의 nc에는 "입력이 끝나면 쓰기 방향 닫기" 옵션(-N)이 없어서예요.

cd "$(dirname "$0")/.." || exit 2

PORT="${PORT:-18000}"
BASE="http://localhost:$PORT"
SUCCESS_TEXT='3 + 16 = 19'
RESULT_TEXT='The answer is'
TMP=$(mktemp -d)

pass=0
fail=0

cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null && wait "$SERVER_PID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

if [ ! -x ./tiny ] || [ ! -x ./cgi-bin/adder ]; then
    echo "tiny 또는 cgi-bin/adder가 없어요 (make 먼저 실행)" >&2
    exit 2
fi

./tiny "$PORT" >"$TMP/server.log" 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 50); do
    curl -s -o /dev/null "$BASE/home.html" && break
    sleep 0.1
done
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "서버가 시작되지 않았어요. 로그:" >&2
    cat "$TMP/server.log" >&2
    exit 2
fi

# send_raw <request bytes (printf 형식)> → stdout으로 응답 원문
# 요청을 다 보낸 뒤 쓰기 방향을 닫고(shutdown) 응답을 끝까지 읽는다.
send_raw() {
    printf "$1" | python3 -c '
import socket, sys
data = sys.stdin.buffer.read()
s = socket.create_connection(("localhost", int(sys.argv[1])), timeout=5)
s.sendall(data)
s.shutdown(socket.SHUT_WR)
out = b""
try:
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        out += chunk
except socket.timeout:
    pass
sys.stdout.buffer.write(out)
' "$PORT"
}

# 응답 원문의 첫 줄에서 상태 코드만 꺼낸다
status_of() {
    head -n 1 | awk '{print $2}'
}

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

# check_status <id> <expected> <actual> [body-must-contain]
check() {
    local id="$1" expected="$2" actual="$3" body="$4" contains="$5"
    if [ "$actual" != "$expected" ]; then
        report "$id" 0 "status $actual (expected $expected)"
    elif [ -n "$contains" ] && [[ "$body" != *"$contains"* ]]; then
        report "$id" 0 "본문에 '$contains'가 없음"
    else
        report "$id" 1
    fi
}

# curl 공통 옵션: 응답 본문은 파일로, 상태 코드는 stdout으로
# -H 'Expect:' : 1KB 넘는 body에 curl이 붙이는 Expect: 100-continue를 끈다
CURL=(curl -s --max-time 5 -H 'Expect:' -o "$TMP/body" -w '%{http_code}')

# TC-FN-01: POST /cgi-bin/adder
code=$("${CURL[@]}" -d 'num1=3&num2=16' "$BASE/cgi-bin/adder")
check TC-FN-01 200 "$code" "$(cat "$TMP/body")" "$SUCCESS_TEXT"

# TC-FN-02: GET /cgi-bin/adder?num1=3&num2=16
code=$("${CURL[@]}" "$BASE/cgi-bin/adder?num1=3&num2=16")
check TC-FN-02 200 "$code" "$(cat "$TMP/body")" "$SUCCESS_TEXT"

# TC-FN-03: PUT → 501
code=$("${CURL[@]}" -X PUT "$BASE/cgi-bin/adder")
check TC-FN-03 501 "$code"

# TC-FN-04: DELETE → 501
code=$("${CURL[@]}" -X DELETE "$BASE/cgi-bin/adder")
check TC-FN-04 501 "$code"

# TC-FN-05: 소문자 get
code=$("${CURL[@]}" -X get "$BASE/home.html")
check TC-FN-05 200 "$code"

# TC-FN-05-1: 소문자 post
code=$("${CURL[@]}" -X post -d 'num1=3&num2=16' "$BASE/cgi-bin/adder")
check TC-FN-05-1 200 "$code" "$(cat "$TMP/body")" "$SUCCESS_TEXT"

# TC-FN-06: 정적 파일에 POST → 405 + Allow: GET
code=$("${CURL[@]}" -D "$TMP/headers" -d 'num1=3&num2=16' "$BASE/home.html")
if [ "$code" != 405 ]; then
    report TC-FN-06 0 "status $code (expected 405)"
elif ! tr -d '\r' <"$TMP/headers" | grep -qi '^Allow: *GET *$'; then
    report TC-FN-06 0 "응답 헤더에 'Allow: GET'이 없음"
else
    report TC-FN-06 1
fi

# TC-FN-07: Content-Length 없는 POST → 411
code=$(send_raw 'POST /cgi-bin/adder HTTP/1.0\r\nHost: localhost\r\n\r\n' | status_of)
check TC-FN-07 411 "$code"

# TC-FN-08: body 8192바이트 → 200
# 앞부분은 정상 인자, 나머지는 채움 문자 (adder는 두 번째 '=' 뒤를 atoi하므로 16으로 읽힘)
{
    printf 'num1=3&num2=16&pad='
    head -c $((8192 - 19)) /dev/zero | tr '\0' 'a'
} >"$TMP/body8192"
size=$(wc -c <"$TMP/body8192" | tr -d ' ')
[ "$size" = 8192 ] || { echo "body8192 크기 오류: $size" >&2; exit 2; }
code=$("${CURL[@]}" --data-binary @"$TMP/body8192" "$BASE/cgi-bin/adder")
check TC-FN-08 200 "$code" "$(cat "$TMP/body")" "$SUCCESS_TEXT"

# TC-FN-09: body 8193바이트 → 413
{ cat "$TMP/body8192"; printf 'a'; } >"$TMP/body8193"
code=$("${CURL[@]}" --data-binary @"$TMP/body8193" "$BASE/cgi-bin/adder")
check TC-FN-09 413 "$code"

# TC-FN-10: Content-Length 20, body 14바이트 후 연결 종료 → 400
code=$(send_raw 'POST /cgi-bin/adder HTTP/1.0\r\nHost: localhost\r\nContent-Length: 20\r\n\r\nnum1=3&num2=16' | status_of)
check TC-FN-10 400 "$code"

# TC-FN-11: Content-Length 0 → 200 + adder 에러 메시지, 다음 요청도 정상
resp=$(send_raw 'POST /cgi-bin/adder HTTP/1.0\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n')
code=$(printf '%s' "$resp" | status_of)
body=$(printf '%s' "$resp" | tr -d '\r' | sed '1,/^$/d')
if [ "$code" != 200 ]; then
    report TC-FN-11 0 "status $code (expected 200)"
elif [[ "$resp" == *"$RESULT_TEXT"* ]]; then
    report TC-FN-11 0 "에러여야 하는데 계산 결과가 나옴"
elif [ -z "$body" ]; then
    report TC-FN-11 0 "adder 에러 메시지가 없음"
else
    code=$("${CURL[@]}" "$BASE/cgi-bin/adder?num1=3&num2=16")
    check TC-FN-11 200 "$code" "$(cat "$TMP/body")" "$SUCCESS_TEXT"
fi

# 모든 요청 후에도 서버가 살아 있어야 한다 (SIGPIPE 등으로 죽지 않았는지)
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "[FAIL] 서버 프로세스가 테스트 중 종료됨" >&2
    fail=$((fail + 1))
fi

echo
echo "server: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
