#!/usr/bin/env bash
# Phase 3 adder 단위 테스트 (서버 없이 환경변수 + stdin으로 실행)
# 케이스: docs/test/phase3/test-cases.md 2장 (TC-ADD)
#
# 실행: tiny 디렉터리에서 make test-adder (또는 bash tests/test_adder.sh)

cd "$(dirname "$0")/.." || exit 2

ADDER=./cgi-bin/adder
SUCCESS_TEXT='3 + 16 = 19'
# 에러 응답이면 계산 결과 문구가 나오면 안 된다
RESULT_TEXT='The answer is'

pass=0
fail=0

if [ ! -x "$ADDER" ]; then
    echo "adder가 없어요: $ADDER (make cgi 먼저 실행)" >&2
    exit 2
fi

# run_adder <stdin> <env assignments...>
# 결과: $OUT, $RC
run_adder() {
    local input="$1"; shift
    OUT=$(printf '%s' "$input" | env -i PATH="$PATH" "$@" "$ADDER" 2>&1)
    RC=$?
}

report() {
    local id="$1" ok="$2" why="$3"
    if [ "$ok" = 1 ]; then
        echo "[PASS] $id"
        pass=$((pass + 1))
    else
        echo "[FAIL] $id: $why"
        echo "       output: $(printf '%s' "$OUT" | head -c 300 | tr '\r\n' '  ')"
        fail=$((fail + 1))
    fi
}

expect_success() {
    local id="$1"
    if [ "$RC" -ne 0 ]; then
        report "$id" 0 "exit code $RC (expected 0)"
    elif [[ "$OUT" != *"$SUCCESS_TEXT"* ]]; then
        report "$id" 0 "본문에 '$SUCCESS_TEXT'가 없음"
    else
        report "$id" 1
    fi
}

expect_error() {
    local id="$1"
    if [ "$RC" -gt 128 ]; then
        report "$id" 0 "시그널로 종료 (exit $RC, signal $((RC - 128)))"
    elif [ "$RC" -ne 0 ]; then
        report "$id" 0 "exit code $RC (expected 0)"
    elif [[ "$OUT" == *"$RESULT_TEXT"* ]]; then
        report "$id" 0 "에러여야 하는데 계산 결과가 나옴"
    elif [ -z "$OUT" ]; then
        report "$id" 0 "출력이 비어 있음 (에러 메시지 필요)"
    else
        report "$id" 1
    fi
}

run_adder 'num1=3&num2=16' REQUEST_METHOD=POST CONTENT_LENGTH=14
expect_success TC-ADD-01

run_adder '' REQUEST_METHOD=GET QUERY_STRING='num1=3&num2=16'
expect_success TC-ADD-02

run_adder 'num1=3&num2=16' REQUEST_METHOD=POST
expect_error TC-ADD-03

run_adder 'num1=3&num2=16' REQUEST_METHOD=POST CONTENT_LENGTH=abc
expect_error TC-ADD-04

run_adder 'num1=3&num2=16' REQUEST_METHOD=POST CONTENT_LENGTH=-1
expect_error TC-ADD-05

run_adder 'num1=3&num2=16' REQUEST_METHOD=POST CONTENT_LENGTH=8193
expect_error TC-ADD-06

run_adder 'num1=3&num2=16' REQUEST_METHOD=POST CONTENT_LENGTH=20
expect_error TC-ADD-07

run_adder 'hello' REQUEST_METHOD=POST CONTENT_LENGTH=5
expect_error TC-ADD-08

run_adder 'num1=3' REQUEST_METHOD=POST CONTENT_LENGTH=6
expect_error TC-ADD-09

run_adder '' REQUEST_METHOD=GET QUERY_STRING=''
expect_error TC-ADD-10

echo
echo "adder: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
