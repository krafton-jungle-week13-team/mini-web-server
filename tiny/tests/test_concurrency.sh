#!/usr/bin/env bash
# 스레드 기반 동시 처리 테스트 (CS:APP 12.3)
#
# 실행: tiny 디렉터리에서 make test-concurrency (또는 bash tests/test_concurrency.sh)
# 포트: PORT 환경변수 (기본 18001, test_server.sh의 18000과 겹치지 않게)

cd "$(dirname "$0")/.." || exit 2

PORT="${PORT:-18001}"
BASE="http://localhost:$PORT"
TMP=$(mktemp -d)

pass=0
fail=0

cleanup() {
    [ -n "$HOLD_PID" ] && kill "$HOLD_PID" 2>/dev/null
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

# TC-CC-01: 요청 줄을 반만 보내고 멈춘 클라이언트가 있어도 다른 요청은 바로 처리된다
# (반복형 서버라면 첫 연결의 readline에서 막혀 curl이 타임아웃난다)
python3 -c '
import socket, sys, time
s = socket.create_connection(("localhost", int(sys.argv[1])))
s.sendall(b"GET /home.ht")
time.sleep(10)
' "$PORT" &
HOLD_PID=$!
sleep 0.3
code=$(curl -s --max-time 2 -o /dev/null -w '%{http_code}' "$BASE/home.html")
if [ "$code" = 200 ]; then
    report TC-CC-01 1
else
    report TC-CC-01 0 "느린 클라이언트가 있는 동안 응답 못 받음 (status $code)"
fi
kill "$HOLD_PID" 2>/dev/null
wait "$HOLD_PID" 2>/dev/null
HOLD_PID=

# TC-CC-02: CGI 요청 30개를 동시에 보내도 모두 올바른 결과가 온다
# (Wait(NULL)이었다면 다른 스레드의 자식을 회수해 ECHILD로 서버가 죽을 수 있다)
N=30
pids=()
for i in $(seq 1 $N); do
    curl -s --max-time 5 "$BASE/cgi-bin/adder?num1=$i&num2=1000" >"$TMP/cgi.$i" &
    pids+=($!)
done
wait "${pids[@]}"          # 인자 없는 wait는 백그라운드 서버까지 기다려서 멈춘다
bad=0
for i in $(seq 1 $N); do
    grep -q "$i + 1000 = $((i + 1000))" "$TMP/cgi.$i" || bad=$((bad + 1))
done
if [ "$bad" = 0 ]; then
    report TC-CC-02 1
else
    report TC-CC-02 0 "$N개 중 $bad개가 틀린 결과 또는 응답 없음"
fi

# TC-CC-03: 요청 도중 연결을 RST로 끊는 클라이언트가 있어도 서버가 살아 있다
# (Rio_readlineb는 ECONNRESET에서 exit()을 불러 서버 전체를 죽인다)
python3 -c '
import socket, struct, sys, time
for _ in range(5):
    s = socket.create_connection(("localhost", int(sys.argv[1])))
    s.sendall(b"GET /home.html HTTP/1.0\r\nHost: x\r\n")
    time.sleep(0.05)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.close()                                  # SO_LINGER 0 → RST 전송
' "$PORT"
sleep 0.3
code=$(curl -s --max-time 2 -o /dev/null -w '%{http_code}' "$BASE/home.html")
if kill -0 "$SERVER_PID" 2>/dev/null && [ "$code" = 200 ]; then
    report TC-CC-03 1
else
    report TC-CC-03 0 "RST 이후 서버가 죽었거나 응답 못 함 (status $code)"
fi

# TC-CC-04: 정적 요청과 CGI 요청을 섞어 동시에 보내도 모두 성공한다
pids=()
for i in $(seq 1 20); do
    if [ $((i % 2)) = 0 ]; then
        curl -s --max-time 5 -o /dev/null -w '%{http_code}\n' "$BASE/home.html" >"$TMP/mix.$i" &
    else
        curl -s --max-time 5 -o /dev/null -w '%{http_code}\n' -d "num1=$i&num2=1" "$BASE/cgi-bin/adder" >"$TMP/mix.$i" &
    fi
    pids+=($!)
done
wait "${pids[@]}"
bad=$(cat "$TMP"/mix.* | grep -vc '^200$')
if [ "$bad" = 0 ]; then
    report TC-CC-04 1
else
    report TC-CC-04 0 "20개 중 $bad개가 200이 아님"
fi

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "[FAIL] 서버 프로세스가 테스트 중 종료됨" >&2
    fail=$((fail + 1))
fi

echo
echo "concurrency: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
