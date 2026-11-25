# 동시성 방식별 성능 비교 (Apache Bench)

이 문서를 읽으면 Tiny를 프로세스, I/O 멀티플렉싱(select), 스레드 풀 방식으로 구현했을 때 처리량과 응답 시간이 어떻게 다른지 알 수 있어요.
같은 조건으로 다시 측정하는 방법도 함께 적었어요.

측정일: 2026-11-25

## 1. 결론

| 방식 | 처리량 상한 | 상한에 닿는 동시 연결 수 | 동시 연결 1개일 때 처리량 |
| --- | --- | --- | --- |
| 스레드 풀 (worker 4) | 약 41,000 req/s | 50개 | 9,490 req/s |
| I/O 멀티플렉싱 (select) | 약 27,000 req/s | 10개 | **11,902 req/s** (가장 빠름) |
| 프로세스 (요청마다 fork) | 약 8,000 req/s | 10개 | 4,317 req/s |

- 동시 연결이 10개 이상일 때는 **스레드 풀 > select > 프로세스** 순서예요. 모든 조건에서 세 방식의 측정 범위(최소~최대)가 겹치지 않아요.
- 동시 연결이 1개일 때는 select가 가장 빨라요. 동시 연결 몇 개부터 순서가 바뀌는지는 [select와 스레드 풀이 뒤집히는 지점](benchmark-crossover.md)에 따로 정리했어요.
- 75번 측정 모두 실패 요청과 2xx가 아닌 응답은 0개였어요.

## 2. 비교한 구현

| 방식 | 브랜치 | 커밋 | 구조 |
| --- | --- | --- | --- |
| 프로세스 | `con-process` | `88f7665` | main이 `accept`하고 연결마다 `fork`. 자식은 요청 하나를 처리하고 `exit`, 부모는 `SIGCHLD` 핸들러로 회수 |
| I/O 멀티플렉싱 | `con-io-multiflexing` | `6181a9c` | 스레드 하나가 `select`로 listenfd와 연결들을 감시하며 처리 |
| 스레드 풀 | `con-thread-pool` | `e49fb45` | main이 `accept`한 connfd를 sbuf(16칸)에 넣고, worker 4개가 꺼내 처리 |

- 세 구현 모두 keep-alive가 없어요. 요청 하나를 처리하면 연결을 닫아요.
- 스레드 풀 브랜치에는 리버스 프록시 코드가 들어 있지만, `-c` 없이 띄웠기 때문에 요청마다 빈 라우트 테이블을 한 번 확인하는 것 말고는 차이가 없어요.

## 3. 측정 환경과 조건

| 항목 | 값 |
| --- | --- |
| 머신 | Intel Core Ultra 7 258V, WSL2 (커널 6.18.58.1, vCPU 8개, 메모리 15GB) |
| 도구 | ApacheBench 2.3 |
| 요청 | `GET /home.html` (정적 파일), keep-alive 없음 (`-k` 안 씀) |
| 동시 연결 수 | 1개, 10개, 50개, 100개, 200개 |
| 측정 한 번 | 10초 (`ab -t 10`) |
| 반복 | 조건마다 5번, 값은 5번의 **중앙값** |
| CPU 고정 | 서버는 코어 0~3, ab는 코어 6 (`taskset`) |
| 서버 로그 | `/dev/null`로 버림 |

결과가 흔들리지 않게 이렇게 측정했어요.

- **요청 수가 아니라 시간으로 쟀어요.** 처음에 요청 20,000개로 쟀을 때는 한 번이 0.5~3초로 끝나서, 같은 조건인데도 결과가 최대 1.7배까지 차이 났어요.
- **세 방식의 측정 순서를 반복마다 섞었어요.** 시간이 지나며 생기는 변화(발열, 호스트의 다른 작업)가 한 방식에만 쏠리지 않게 하려고요.
- **측정마다 접속 주소를 바꿨어요** (127.0.0.2, 127.0.0.3, …). 끝난 연결이 60초 동안 TIME_WAIT로 남는데, 처음 측정 때는 약 28,000개가 쌓여 쓸 수 있는 포트를 거의 다 채웠어요. 주소가 다르면 다음 측정의 연결과 겹치지 않아요.
- **측정 사이에 3초씩 쉬고**, 시작 전에 방식마다 3초씩 워밍업했어요.
- **ab가 병목이 아닌지 확인했어요.** ab 하나로 스레드 풀을 쟀을 때 41,153 req/s였고, ab 두 개를 동시에 돌려도 합계가 약 40,000 req/s로 같았어요. 그래서 이 처리량은 ab가 아니라 서버의 한계예요.

## 4. 결과

### 처리량 (req/s, 5번 중 중앙값)

| 동시 연결 수 | 프로세스 | select | 스레드 풀 |
| --- | --- | --- | --- |
| 1개 | 4,317 | **11,902** | 9,490 |
| 10개 | 7,988 | 27,376 | **35,990** |
| 50개 | 7,599 | 26,493 | **40,922** |
| 100개 | 8,122 | 27,190 | **41,316** |
| 200개 | 8,155 | 27,035 | **41,189** |

### 평균 응답 시간 (ms, 5번 중 중앙값)

| 동시 연결 수 | 프로세스 | select | 스레드 풀 |
| --- | --- | --- | --- |
| 1개 | 0.232 | 0.084 | 0.105 |
| 10개 | 1.252 | 0.365 | 0.278 |
| 50개 | 6.580 | 1.887 | 1.222 |
| 100개 | 12.312 | 3.678 | 2.420 |
| 200개 | 24.525 | 7.398 | 4.856 |

### 응답 시간 분포 (ms, p50 / p90 / p99)

ab는 백분위를 1ms 단위로 내림해서 보여 줘요. 0은 1ms 미만이라는 뜻이에요.

| 동시 연결 수 | 프로세스 | select | 스레드 풀 |
| --- | --- | --- | --- |
| 1개 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| 10개 | 1 / 1 / 2 | 0 / 0 / 1 | 0 / 0 / 1 |
| 50개 | 6 / 7 / 9 | 2 / 2 / 3 | 1 / 2 / 2 |
| 100개 | 12 / 13 / 16 | 4 / 4 / 5 | 2 / 3 / 4 |
| 200개 | 24 / 26 / 31 | 7 / 8 / 9 | 5 / 5 / 7 |

### 측정의 흔들림

변동계수(CV)는 5번 측정값의 표준편차를 평균으로 나눈 값이에요. 5%를 넘으면 굵게 표시했어요.

| 동시 연결 수 | 방식 | 5번 측정값 (req/s) | CV |
| --- | --- | --- | --- |
| 1개 | 프로세스 | 4,413 · 4,274 · 4,282 · 4,355 · 4,317 | 1.2% |
| 1개 | select | 11,957 · 11,902 · 11,946 · 11,820 · 11,613 | 1.1% |
| 1개 | 스레드 풀 | 9,228 · 9,517 · 9,490 · 9,597 · 9,392 | 1.3% |
| 10개 | 프로세스 | 7,752 · 8,195 · 7,988 · 7,756 · 8,097 | 2.2% |
| 10개 | select | 27,386 · 26,979 · 27,107 · 27,376 · 27,427 | 0.7% |
| 10개 | 스레드 풀 | 36,128 · 36,141 · 35,990 · 35,932 · 35,229 | 0.9% |
| 50개 | 프로세스 | 7,896 · 6,481 · 7,776 · 7,332 · 7,599 | **6.8%** |
| 50개 | select | 27,205 · 26,439 · 26,493 · 26,650 · 25,381 | 2.2% |
| 50개 | 스레드 풀 | 41,348 · 41,601 · 40,917 · 40,922 · 38,955 | 2.3% |
| 100개 | 프로세스 | 7,067 · 8,122 · 8,098 · 8,147 · 8,291 | **5.6%** |
| 100개 | select | 27,209 · 27,066 · 27,190 · 27,341 · 27,098 | 0.4% |
| 100개 | 스레드 풀 | 40,794 · 41,230 · 41,462 · 41,728 · 41,316 | 0.7% |
| 200개 | 프로세스 | 8,155 · 8,200 · 8,027 · 8,229 · 7,997 | 1.1% |
| 200개 | select | 27,035 · 27,043 · 26,974 · 27,127 · 26,999 | 0.2% |
| 200개 | 스레드 풀 | 41,444 · 41,189 · 40,302 · 41,242 · 40,558 | 1.1% |

CV가 5%를 넘은 두 조건은 모두 프로세스 방식이에요. 5번 중 한 번만 유독 느렸고(6,481과 7,067), 나머지 4번은 서로 비슷했어요. `fork`는 호스트 상태에 민감해서 생긴 일시적인 차이로 보여요.

## 5. 해석

### 프로세스: 약 8,000 req/s에서 멈춰요

- 요청마다 프로세스를 만들고(`fork`), 끝내고(`exit`), 회수(`SIGCHLD` → `waitpid`)하는 비용이 들어요. 이 비용이 요청 처리 자체보다 커요.
- `accept`와 `fork`는 부모 프로세스 하나가 차례로 해요. 그래서 동시 연결을 늘려도 처리량이 오르지 않아요.
- 대신 연결이 줄을 서서 기다리기 때문에, 평균 응답 시간이 동시 연결 수에 비례해서 늘어요. (동시 연결 10개일 때 1.3ms, 200개일 때 24.5ms)

### select: 동시 연결이 1개일 때는 가장 빠르고, 약 27,000 req/s에서 멈춰요

- 스레드 하나가 `accept`부터 응답까지 직접 처리해요. 다른 스레드를 깨우거나 일을 넘기는 비용이 없어서, 연결이 하나뿐일 때 가장 빨라요.
- 하지만 CPU 코어를 하나만 써요. 코어 하나가 다 차는 동시 연결 10개부터는 연결을 더 늘려도 처리량이 그대로예요.

### 스레드 풀: 동시 연결이 10개 이상일 때 가장 빠르고, 약 41,000 req/s에서 멈춰요

- worker 4개가 코어 4개에서 동시에 처리해서 select보다 약 1.5배 빨라요.
- 동시 연결이 1개일 때는 select보다 느려요. main이 `accept`한 연결을 sbuf에 넣고 worker를 깨워 넘기는 과정이 요청마다 더해지기 때문이에요.
- 동시 연결 50개부터 처리량이 더 오르지 않아요. worker 수(4)와 서버에 준 코어 수(4)가 한계예요.

## 6. 한계

- **한 머신에서 측정했어요.** ab와 서버가 같은 WSL 가상 머신에서 돌아요. 코어는 나눠 줬지만 메모리, 커널, 호스트(Windows)의 영향은 공유해요. 절대값보다 세 방식 사이의 상대 비교로 봐야 해요.
- **작은 정적 파일 하나만 쟀어요.** 응답이 크거나(예: `godzilla.gif`), CGI처럼 요청마다 프로그램을 실행하는 경우에는 결과가 달라질 수 있어요.
- **스레드 풀은 worker 4개로만 쟀어요.** `NTHREADS`나 서버에 주는 코어 수를 바꾸면 상한이 달라질 수 있어요. select와 프로세스 구현도 각 브랜치의 기본 설정 그대로예요.
- **keep-alive가 없는 조건이에요.** 연결을 다시 쓰는 경우에는 연결을 맺고 끊는 비용이 빠져서, 방식 사이의 차이가 달라질 수 있어요.

## 7. 다시 측정하기

부록의 스크립트를 WSL에서 실행해요. 스크립트는 세 브랜치를 `git archive`로 `/tmp/bench3/`에 꺼내 빌드해요. 지금 작업 중인 브랜치나 파일은 건드리지 않아요.

```bash
sudo apt install -y apache2-utils      # ab가 없으면
git fetch origin                       # 팀원 브랜치를 최신으로 (Windows 쪽에서 실행해도 됨)
bash bench3.sh                         # 기본값: 10초 x 5회, 동시 연결 1·10·50·100·200개
```

설정은 환경 변수로 바꿔요.

| 변수 | 기본값 | 의미 |
| --- | --- | --- |
| `DUR` | `10` | 측정 한 번의 길이(초). (예: `DUR=20`) |
| `REPEAT` | `5` | 같은 조건을 몇 번 잴지. (예: `REPEAT=3`) |
| `CS` | `"1 10 50 100 200"` | 동시 연결 수 목록. 공백으로 구분해요. (예: `CS="10 100"`) |
| `FILE` | `home.html` | 요청할 경로. 앞의 `/`는 빼요. (예: `FILE=godzilla.gif`, `FILE='cgi-bin/adder?num1=1&num2=2'`) |
| `REST` | `3` | 측정 사이에 쉬는 시간(초). (예: `REST=5`) |
| `SERVER_CPUS` | `0-3` | 서버를 묶을 코어. `taskset -c` 형식이에요. (예: `SERVER_CPUS=0-7`) |
| `AB_CPU` | `6` | ab를 묶을 코어 하나. (예: `AB_CPU=7`) |

스크립트 맨 위의 `REPO`는 저장소의 WSL 경로예요. 자기 환경에 맞게 바꿔요.
세 방식 x 동시 연결 5종류 x 5번 = 75번 측정에 약 17분 걸려요.
끝나면 조건별 중앙값, 최소, 최대, CV, p50, p99 표를 출력하고, 원본은 `/tmp/bench3/results.csv`에 남겨요.

<details>
<summary>부록: bench3.sh</summary>

```bash
#!/usr/bin/env bash
# 세 가지 동시성 방식(프로세스 / select / 스레드 풀)을 같은 조건으로 빌드하고 ab로 비교한다. WSL에서 실행.
#
# 안정적으로 재기 위해:
#   - 요청 수가 아니라 시간(DUR초)으로 측정한다
#   - 같은 조건을 REPEAT번 재고 중앙값·최소·최대·변동계수(CV)를 낸다
#   - 반복마다 세 방식의 순서를 섞는다
#   - 서버는 SERVER_CPUS 코어에만, ab는 AB_CPU 코어에만 묶는다 (taskset)
#   - 측정마다 접속 주소를 127.0.0.2, .3, ...으로 바꾼다.
#     끝난 연결이 TIME_WAIT로 남아도 주소가 다르면 다음 측정의 연결과 겹치지 않는다
#   - 측정 사이에 REST초 쉰다
#
#   DUR=10 REPEAT=5 CS="1 10 50 100 200" FILE=home.html REST=3 bash bench3.sh
REPO="/mnt/c/Users/dhrho/문서/JungleGithub/mini-web-server"
B=/tmp/bench3
DUR=${DUR:-10}
REPEAT=${REPEAT:-5}
CS=${CS:-"1 10 50 100 200"}
FILE=${FILE:-home.html}
REST=${REST:-3}
SERVER_CPUS=${SERVER_CPUS:-0-3}
AB_CPU=${AB_CPU:-6}
OUT=${OUT:-$B/results.csv}

NAMES=(process select threadpool)
declare -A REF=([process]=origin/con-process [select]=origin/con-io-multiflexing [threadpool]=con-thread-pool)
declare -A PORT=([process]=18401 [select]=18402 [threadpool]=18403)

command -v ab >/dev/null || { echo "ab가 없어요: sudo apt install -y apache2-utils"; exit 2; }
pkill -x tiny 2>/dev/null
rm -rf "$B" && mkdir -p "$B"

# git archive는 저장소에 들어 있는 내용(LF)을 꺼내므로 CRLF 변환이 필요 없다
for k in "${NAMES[@]}"; do
    mkdir -p "$B/$k"
    git -c safe.directory='*' -C "$REPO" archive "${REF[$k]}" tiny | tar -x -C "$B/$k" \
        || { echo "$k: ${REF[$k]}를 꺼내지 못함 (git fetch origin 했는지 확인)"; exit 2; }
    make -C "$B/$k/tiny" >/dev/null 2>&1 || { echo "$k 빌드 실패"; exit 2; }
    # 요청마다 printf하는 로그는 버린다 (터미널 출력이 결과를 왜곡함)
    (cd "$B/$k/tiny" && exec taskset -c "$SERVER_CPUS" ./tiny "${PORT[$k]}" </dev/null >/dev/null 2>&1) &
done
trap 'pkill -x tiny' EXIT
sleep 1

ipn=2
next_ip() { echo "127.0.0.$ipn"; ipn=$(( ipn % 250 + 2 )); }

for k in "${NAMES[@]}"; do
    curl -s -o /dev/null "http://127.0.0.1:${PORT[$k]}/$FILE" || { echo "$k 서버가 응답하지 않음"; exit 2; }
    taskset -c "$AB_CPU" ab -q -r -t 3 -n 100000000 -c 50 "http://$(next_ip):${PORT[$k]}/$FILE" >/dev/null 2>&1   # 워밍업
done

refs=""
for k in "${NAMES[@]}"; do refs="$refs $k=$(git -c safe.directory='*' -C "$REPO" rev-parse --short "${REF[$k]}")"; done
total=$(( $(wc -w <<<"$CS") * REPEAT * 3 ))
echo "$FILE, 측정당 ${DUR}초 x ${REPEAT}회, 서버 CPU $SERVER_CPUS, ab CPU $AB_CPU, 브랜치:$refs"
echo "측정 $total번, 예상 약 $(( total * (DUR + REST + 1) / 60 ))분"

echo "method,c,run,rps,mean_ms,p50,p90,p99,failed,non2xx,complete" >"$OUT"
done_n=0
for c in $CS; do
    for r in $(seq 1 "$REPEAT"); do
        for k in $(printf '%s\n' "${NAMES[@]}" | shuf); do
            sleep "$REST"
            out=$(taskset -c "$AB_CPU" ab -q -r -t "$DUR" -n 100000000 -c "$c" \
                  "http://$(next_ip):${PORT[$k]}/$FILE" 2>&1)
            g() { awk "$1" <<<"$out"; }
            line="$k,$c,$r,$(g '/^Requests per second/ {print $4}'),$(g '/^Time per request.*\(mean\)/ {print $4}')"
            line="$line,$(g '/^  50%/ {print $2}'),$(g '/^  90%/ {print $2}'),$(g '/^  99%/ {print $2}')"
            line="$line,$(g '/^Failed requests/ {print $3}'),$(g 'BEGIN{n=0} /^Non-2xx/ {n=$3} END{print n}'),$(g '/^Complete requests/ {print $3}')"
            echo "$line" >>"$OUT"
            done_n=$((done_n + 1))
            printf '[%3d/%d] %-10s c=%-3s run %d: %s req/s\n' "$done_n" "$total" "$k" "$c" "$r" "$(cut -d, -f4 <<<"$line")"
        done
    done
done

python3 - "$OUT" <<'EOF'
import csv, statistics as st, sys
rows = list(csv.DictReader(open(sys.argv[1])))
order = ["process", "select", "threadpool"]
cs = sorted({int(r["c"]) for r in rows})
print()
print(f'{"c":>4} {"방식":<10} {"req/s 중앙값":>12} {"최소":>9} {"최대":>9} {"CV":>6}  {"p50":>4} {"p99":>4}  {"실패":>4} {"non2xx":>6}')
for c in cs:
    for m in order:
        g = [r for r in rows if r["method"] == m and int(r["c"]) == c and r["rps"]]
        if not g:
            continue
        rps = [float(r["rps"]) for r in g]
        cv = st.pstdev(rps) / st.mean(rps) * 100 if len(rps) > 1 else 0
        p50 = st.median(float(r["p50"]) for r in g)
        p99 = st.median(float(r["p99"]) for r in g)
        fail = sum(int(r["failed"] or 0) for r in g)
        non2xx = sum(int(r["non2xx"] or 0) for r in g)
        flag = "  <- CV 5% 넘음" if cv > 5 else ""
        print(f'{c:>4} {m:<10} {st.median(rps):>12,.0f} {min(rps):>9,.0f} {max(rps):>9,.0f} {cv:>5.1f}%  {p50:>4.0f} {p99:>4.0f}  {fail:>4} {non2xx:>6}{flag}')
    print()
EOF
echo "원본: $OUT"
```

</details>
