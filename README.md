# mini-web-server

CS:APP의 Tiny 웹 서버에 POST 요청, 리버스 프록시, 동시 연결 지원을 더한 학습용 서버입니다.

- **프로토콜**: HTTP/1.0. 요청 하나에 응답한 뒤 연결을 닫습니다(keep-alive 없음).
- **메서드**: GET, POST (그 외는 `501 Not Implemented`)
- **미지원**: HTTPS(TLS), HTTP/1.1 이상

## 실행
`tiny/` 디렉터리에서 빌드하고 실행합니다. 서버는 실행한 디렉터리를 문서 루트로 씁니다.

```bash
cd tiny && make
./tiny 8000                       # 웹 서버로만 실행
./tiny 8000 -c routes.conf        # 웹 서버 + 리버스 프록시로 실행
```

다른 터미널에서 요청을 보내 확인합니다.

```bash
curl http://localhost:8000/                                  # home.html
curl "http://localhost:8000/cgi-bin/adder?num1=3&num2=16"    # CGI
```

리버스 프록시 실행은 [리버스 프록시](#리버스-프록시)를 참고하세요.

## 기능

### 정적 콘텐츠
경로에 `cgi-bin`이 없는 요청은 실행한 디렉터리 아래에서 파일을 찾아 보냅니다.

- **경로**: `/`로 끝나면 그 디렉터리의 `home.html`을 보냅니다. 파일이 없으면 404를 응답합니다.
- **메서드**: GET만 지원합니다. POST는 `405 Method Not Allowed`(`Allow: GET`)로 응답합니다.
- **요청 데이터**: 쿼리 스트링을 처리하지 않습니다. `/home.html?a=1`은 404를 응답합니다.
- **응답 헤더**: 서버가 파일 확장자로 MIME 타입(`Content-Type`)을 정합니다. 텍스트 파일에는 `; charset=utf-8`을 붙입니다.

  | 확장자 | MIME 타입 |
  |---|---|
  | `html`, `htm` | `text/html` |
  | `css` | `text/css` |
  | `js` | `text/javascript` |
  | `txt` | `text/plain` |
  | `png` | `image/png` |
  | `jpg`, `jpeg` | `image/jpeg` |
  | `gif` | `image/gif` |
  | 그 외 | `application/octet-stream` |

- **예시**

  ```bash
  curl http://localhost:8000/home.html
  ```

### 동적 콘텐츠 (CGI)
경로에 `cgi-bin`이 있는 요청은 해당 프로그램을 실행하고, 출력을 그대로 응답으로 보냅니다.

- **경로**: `/cgi-bin/adder`면 `./cgi-bin/adder`를 실행합니다. 파일이 없으면 404, 실행 권한이 없으면 403을 응답합니다.
- **메서드**: GET과 POST를 지원합니다.
- **요청 데이터**: 프로그램에 다음 값을 넘깁니다.

  | 환경변수 / 입력 | 내용 | 설정되는 경우 |
  |---|---|---|
  | `REQUEST_METHOD` | `GET` 또는 `POST` | 항상 |
  | `QUERY_STRING` | `?` 뒤의 문자열 (예: `num1=3&num2=16`) | 항상 (없으면 빈 문자열) |
  | `CONTENT_LENGTH` | body 길이(바이트) | POST |
  | 표준 입력(stdin) | 요청 body (최대 8KB) | POST |

- **응답 헤더**: 서버는 상태 줄과 `Server` 헤더만 보내므로, 프로그램이 나머지 헤더와 빈 줄을 직접 출력해야 합니다(`cgi-bin/adder.c` 참고).
- **예시**

  ```bash
  curl "http://localhost:8000/cgi-bin/adder?num1=3&num2=16"     # GET
  curl -d "num1=3&num2=16" http://localhost:8000/cgi-bin/adder   # POST
  ```

### 리버스 프록시
tiny는 웹 서버이면서 리버스 프록시로도 동작합니다. `routes.conf`의 규칙에 맞는 요청은 백엔드로 넘기고, 나머지는 직접 응답합니다.

```bash
./tiny 8001 &                     # 백엔드 1
./tiny 8003 &                     # 백엔드 2
./tiny 8000 -c routes.conf        # 프록시
```

`routes.conf`에는 어떤 경로를 어느 백엔드로 보낼지 적습니다.

```
# prefix  strip  backends
/api      no     127.0.0.1:8001 127.0.0.1:8003
/calc/    yes    127.0.0.1:8001 127.0.0.1:8003
```

- `/api`로 시작하는 요청은 경로를 그대로 두고 8001, 8003에 번갈아 보냅니다(라운드로빈).
- `/calc/`로 시작하는 요청은 `/calc`를 떼고 보냅니다 (`/calc/add` → `/add`). 백엔드가 prefix 없는 경로를 쓸 때 사용합니다.
- 백엔드로 보낼 때 클라이언트와 프록시 사이의 연결에만 해당하는 헤더는 빼고, 원래 클라이언트 IP를 `X-Forwarded-For`에 담아 보냅니다.

## 에러 응답
| 코드 | 상황 |
|---|---|
| 400 | 헤더 형식이 잘못됨, body가 Content-Length보다 짧은 채로 연결이 끊김 |
| 403 | 파일 읽기 권한이나 CGI 실행 권한이 없음 |
| 404 | 요청한 파일이 없음 |
| 405 | 정적 콘텐츠에 POST 요청 |
| 411 | POST 요청에 Content-Length가 없음 |
| 413 | body가 8KB를 넘음 |
| 414 | 백엔드로 보낼 경로가 너무 김 |
| 500 | 프록시 요청을 만들 메모리가 부족함 |
| 501 | GET, POST 외의 메서드 |
| 502 | 백엔드에 연결하지 못했거나 백엔드가 응답 없이 연결을 닫음 |

## 동시성
I/O 멀티플렉싱으로 프로세스 하나가 여러 연결을 함께 처리합니다.

### 구현 방식
- **select**: 여러 연결을 한 번에 지켜보다가, 데이터가 도착한 연결만 알려 받습니다.
- **이벤트 루프**: select를 반복해서 부르고, 준비된 연결이 생길 때마다 처리합니다.
- **연결별 상태 기계**: 요청이 나눠서 도착해도, 연결마다 어디까지 받았는지 기억해 두고 다른 연결을 먼저 처리합니다. 나머지가 도착하면 이어서 읽습니다.

### 다른 방식과 비교
| 방식 | 구조 | 예 |
|---|---|---|
| 반복(iterative) | 연결을 하나씩 순서대로 처리 | 원래 Tiny |
| 프로세스 | 연결마다 `fork` | Apache prefork |
| 스레드 | 연결마다 스레드 | Apache worker |
| select 이벤트 루프 | 프로세스 하나가 여러 연결을 처리 | mini-web-server |
| epoll 이벤트 루프 | select와 같지만 연결 수 제한이 없음 | nginx |

<!-- 벤치마크 결과 추가 예정 -->

### 한계와 개선 방향
- **블로킹 I/O**: 소켓은 모두 블로킹 모드라, 아래 경우에는 끝날 때까지 다른 연결이 기다립니다.
  - 요청: 요청 줄이나 헤더 한 줄이 중간에 끊겨 도착할 때 (예: `GET /home`까지만 오고 나머지가 늦게 옴)
  - 응답: 큰 파일 전송, CGI 실행, 프록시 중계 (예: 백엔드가 3초 늦게 응답하면 다른 요청도 3초 대기)
  - 개선: 소켓을 논블로킹 모드로 바꾸고, 백엔드 소켓과 CGI 파이프도 select로 지켜봅니다.
- **동시 연결 수 상한**: select는 fd 번호가 `FD_SETSIZE`(보통 1024) 미만인 연결만 다룰 수 있습니다.
  - 개선: 연결 수 제한이 없는 epoll(Linux)이나 kqueue(macOS)로 바꿉니다.
- **단일 코어**: 프로세스 하나로 동작해서 CPU 코어를 하나만 사용합니다.
  - 개선: 코어 수만큼 워커 프로세스를 띄워 연결을 나눠 받습니다.
