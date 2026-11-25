#!/bin/bash
# 데모 실행: 백엔드 2개(:8001, :8002)를 백그라운드로, 프록시(:8000)를 포그라운드로 띄운다.
# Ctrl+C로 프록시를 끄면 백엔드도 함께 종료된다.
#
#   bash demo/run-demo.sh
#   → 브라우저에서 http://localhost:8000/

set -e
DEMO="$(cd "$(dirname "$0")" && pwd)"
TINY_DIR="$DEMO/../tiny"
TINY="$TINY_DIR/tiny"

# 1. 빌드 (tiny 바이너리가 없거나 소스가 더 최신이면)
make -C "$TINY_DIR" tiny >/dev/null

# 2. 포트 확인
for port in 8000 8001 8002; do
    if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
        echo "포트 $port가 이미 사용 중이에요. 그 프로세스를 먼저 종료해 주세요." >&2
        exit 1
    fi
done

# 3. 권한 준비 (git은 000 권한을 저장하지 않으므로 실행할 때마다 맞춘다)
chmod 000 "$DEMO/site/secret.html"
chmod +x "$DEMO/site/cgi-bin/"*
mkdir -p "$DEMO/logs"

# 4. 백엔드 실행 (각 폴더가 웹 루트가 된다)
(cd "$DEMO/backend1" && exec "$TINY" 8001) >"$DEMO/logs/backend1.log" 2>&1 &
B1=$!
(cd "$DEMO/backend2" && exec "$TINY" 8002) >"$DEMO/logs/backend2.log" 2>&1 &
B2=$!

cleanup() {
    kill "$B1" "$B2" 2>/dev/null || true
    wait "$B1" "$B2" 2>/dev/null || true
    chmod 644 "$DEMO/site/secret.html"     # git status가 지저분해지지 않게 되돌림
    echo "백엔드를 종료했어요."
}
trap cleanup EXIT

echo "backend1  http://localhost:8001  (로그: demo/logs/backend1.log)"
echo "backend2  http://localhost:8002  (로그: demo/logs/backend2.log)"
echo "proxy     http://localhost:8000  ← 브라우저에서 여기로 접속"
echo "종료: Ctrl+C"
echo

# 5. 프록시 실행 (site/가 웹 루트, /api/*는 백엔드로)
cd "$DEMO/site"
"$TINY" 8000 -c "$DEMO/routes.conf"
