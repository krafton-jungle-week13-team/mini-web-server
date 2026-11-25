# 리버스 프록시

이 문서를 읽으면 Tiny를 리버스 프록시로 띄우고, 요청이 어느 백엔드로 어떻게 전달되는지, 실패하면 어떤 응답이 오는지 알 수 있어요.

관련 이슈: #27 (하위 #28~31)

## 1. 실행 방법

```bash
./tiny 8080 -c routes.conf
```

`-c`가 없으면 지금까지처럼 정적 파일과 CGI만 처리해요.
설정 파일에 오류가 있으면 줄 번호와 이유를 출력하고 시작하지 않아요. (예: `routes:3: strip은 yes 또는 no`)

## 2. 라우팅 규칙

`route.c`, `route.h`, `routes.conf`, `tests/test_route.c`는 `con-io-proxy` 브랜치(donghwanoh97)의 파일을 내용 그대로 가져왔어요.
두 브랜치가 같은 파일을 쓰니 나중에 합칠 때 충돌이 나지 않아요.

```
# prefix  strip(yes|no)  host:port [host:port ...]
/api      no     127.0.0.1:8001 127.0.0.1:8003
/img/     yes    127.0.0.1:8002
```

- `prefix`: 이 경로로 시작하는 요청을 전달해요. 경로 조각 단위로 비교해서 `/api`는 `/api`, `/api/users`, `/api?q=1`에 맞고 `/apix`에는 맞지 않아요. 여러 규칙이 맞으면 가장 긴 prefix를 골라요.
- `strip`: `yes`면 prefix를 떼고 백엔드에 보내요. (예: `/img/cat.png` → `/cat.png`)
- `host:port`: 백엔드 주소예요. 여러 개면 요청마다 번갈아 보내요(round-robin). 규칙은 최대 16개, 규칙 하나의 백엔드는 최대 8개예요.

`route_pick`은 round-robin 차례를 바꾸기 때문에, 여러 worker가 동시에 부르면 차례가 꼬일 수 있어요.
`route.c`는 그대로 두고 `tiny.c`에서 `rr_lock` mutex로 감싸요.

## 3. 처리 흐름

`doit`에서 헤더를 읽은 직후 `route_match`로 규칙을 찾아요.

| 순서 | 단계 | 규칙에 맞음 | 맞지 않음 |
| --- | --- | --- | --- |
| 1 | 메서드 검사 | GET/POST만 (501) | 같음 |
| 2 | 헤더 파싱 | 400 | 같음 |
| 3 | 정적 파일에 POST | 검사 안 함 | 405 |
| 4 | Content-Length 없는 POST, 너무 큰 body, 짧은 body | 411, 413, 400 | 같음 |
| 5 | 처리 | `proxy_request` | `serve_static` / `serve_dynamic` |

## 4. 백엔드로 보내는 요청

```
GET /path?x=1 HTTP/1.0                 ← 클라이언트 메서드 + 고친 경로, 버전은 항상 1.0
Host: localhost:8080                   ← 클라이언트 헤더 그대로 (hop-by-hop 제외)
User-Agent: curl/8.18.0
X-Forwarded-For: 10.0.0.1, 127.0.0.1   ← 기존 값 뒤에 클라이언트 IP를 덧붙임
Connection: close                      ← 프록시가 붙임

(body)
```

- **hop-by-hop 헤더는 빼요.** `Connection`, `Keep-Alive`, `Proxy-Connection`, `TE`, `Trailer`, `Transfer-Encoding`, `Upgrade`는 클라이언트와 프록시 사이의 연결에만 의미가 있어요. (RFC 9110 7.6.1)
- **`Host`가 없으면** 백엔드 주소로 붙여요. (예: `Host: 127.0.0.1:8001`)
- **헤더 원문은 `read_requesthdrs`가 남겨 둬요.** `req_headers_t`의 `raw`에 담기고, 합쳐서 `MAXBUF`(8KB)를 넘으면 `raw_overflow`가 1이 돼요. 이때 프록시는 431을 보내요. 프록시가 아닌 요청에는 영향이 없어요.

## 5. 클라이언트로 보내는 응답

- 백엔드에 `HTTP/1.0`과 `Connection: close`로 보냈으니, 백엔드가 연결을 닫으면 응답이 끝난 거예요.
- 백엔드 응답은 상태줄과 헤더까지 바이트 그대로 클라이언트에 전달해요.
- 백엔드 응답을 다 전달하면 클라이언트 연결을 닫아요. 이 서버는 keep-alive를 지원하지 않아서, 클라이언트가 HTTP/1.1로 요청해도 응답마다 연결을 닫아요.

## 6. 실패 처리

| 상황 | 응답 |
| --- | --- |
| 백엔드에 연결할 수 없음 (포트에 서버 없음 등) | 502 Bad Gateway |
| 백엔드가 응답 없이 연결을 닫음 | 502 Bad Gateway |
| 요청을 백엔드에 보내다 실패 | 502 Bad Gateway |
| 헤더가 너무 커서 전달할 수 없음 | 431 Request Header Fields Too Large |
| `strip`으로 고친 경로가 너무 김 | 414 URI Too Long |

- **백엔드 응답을 기다리는 시간에는 제한이 없어요.** 연결은 됐지만 응답하지 않는 백엔드에 보낸 요청은 백엔드가 응답하거나 연결을 닫을 때까지 기다려요. 그래서 504 Gateway Timeout은 보내지 않아요.
- **응답이 일부 나간 뒤에 실패하면** 502를 보낼 수 없어서 연결만 끊어요. 클라이언트는 잘린 응답을 받아요.
- **죽은 백엔드를 건너뛰지 않아요.** 백엔드 하나가 죽으면 그 차례의 요청은 502를 받아요. 이슈 #27의 확인 항목("백엔드 하나를 kill하면 502")과 맞췄어요.

## 7. 한계

- Forward Proxy와 캐싱(이슈의 선택 항목)은 구현하지 않았어요.
- 백엔드 연결을 재사용하지 않아요. 요청마다 새로 연결해요.
- 요청 body는 Content-Length 방식만 받고 `MAXBODY`(8KB)까지예요. Content-Length 없이 chunked로 보낸 POST는 411이에요.
- 백엔드가 응답하는 동안 worker 하나가 붙잡혀요. 시간 제한이 없어서, 응답하지 않는 백엔드로 가는 요청이 worker 수(`NTHREADS`, 기본 4)만큼 쌓이면 서버 전체가 멈춰요.
- 응답이 없는 주소(패킷을 버리는 IP 등)로 연결하면 `connect`가 커널 기본 시간(Linux에서 보통 1~2분)이 지나야 실패하고 502가 나가요.

## 8. 테스트

| 테스트 | 위치 |
| --- | --- |
| 라우트 매칭, round-robin, 경로 재작성 (TC-RT, TC-RR, TC-RW) | `tests/test_route.c` (`make test-route`) |
| 헤더 원문 보존 (TC-RAW-01~02) | `tests/test_http_parse.c` |
| 프록시 동작 (TC-PX-01~16) | `tests/test_proxy.sh` (`make test-proxy`) |

`test_proxy.sh`는 tiny 백엔드 두 개("Backend A", "Backend B")와 가짜 백엔드 세 개(`tests/fake_backend.py`의 echo, slow, empty)를 띄워요.

| 케이스 | 확인하는 것 |
| --- | --- |
| TC-PX-01 | 백엔드 두 개에 번갈아 전달 |
| TC-PX-02 | 규칙에 맞지 않는 경로는 직접 응답 |
| TC-PX-03, 04 | strip, POST body, 쿼리 전달 |
| TC-PX-05 | 바이너리 응답을 바이트 그대로 전달 |
| TC-PX-06, 07, 09 | 죽은 백엔드, 하나만 죽은 경우, 응답 없이 닫는 백엔드 → 502 |
| TC-PX-08 | 응답하지 않는 백엔드는 계속 기다리고(504 없음), 그동안 다른 요청은 처리됨 |
| TC-PX-10, 11 | hop-by-hop 제거, `X-Forwarded-For`, `Host` 보존과 보충 |
| TC-PX-12 | HTTP/1.1이어도 프록시 응답 뒤 연결 종료 |
| TC-PX-13 | 규칙에 맞는 POST도 Content-Length가 없으면 411 |
| TC-PX-14 | 동시 요청 20개가 섞이지 않음 (round-robin mutex) |
| TC-PX-15, 16 | 잘못된 설정 파일, 없는 설정 파일 → 시작하지 않음 |

## 9. 직접 확인하기

```bash
# 터미널 1, 2: 백엔드 두 개 (각자 다른 home.html이 있는 폴더에서)
cd /tmp/a && /path/to/tiny 8001
cd /tmp/b && /path/to/tiny 8002

# 터미널 3: 프록시
printf '/web yes 127.0.0.1:8001 127.0.0.1:8002\n' > /tmp/routes.conf
./tiny 8080 -c /tmp/routes.conf

# 터미널 4: 새로고침할 때마다 번갈아 나오는지, 백엔드 하나를 kill하면 502가 오는지
curl -s localhost:8080/web/home.html
```
