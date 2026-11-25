## 현재 지원 기능

이벤트 기반 I/O 멀티플렉싱으로 동작하는 HTTP/1.0 웹 서버입니다.

### 구조
- **select 기반 이벤트 루프를 쓰는 단일 프로세스 서버**
  - 연결마다 상태 기계(요청 줄 → 헤더 → body)를 둡니다. 요청이 아직 다 도착하지 않았으면 select로 돌아가 다른 연결부터 처리합니다.
  - 요청 하나에 응답한 뒤 연결을 닫습니다 (`Connection: close`).
- **HTTP/1.0**: 응답 버전은 항상 `HTTP/1.0`이고, 메서드는 GET과 POST만 지원합니다.
- **주요 기능**: 정적 파일 서빙, CGI, 리버스 프록시

### 실행
```bash
cd tiny && make
./tiny <port>                       # 웹 서버로만 실행
./tiny <port> -c routes.conf        # 웹 서버 + 리버스 프록시로 실행
```

### 정적 파일 서빙
- `/`로 끝나는 경로를 요청하면 그 디렉터리의 `home.html`을 보냅니다.
- 파일 확장자를 보고 Content-Type을 정합니다 (html, css, js, png, jpg, gif, txt / 그 외는 `application/octet-stream`).
- 정적 파일 경로로 POST 요청이 오면 `405 Method Not Allowed`(`Allow: GET`)를 응답합니다.

### 동적 콘텐츠 (CGI)
- `/cgi-bin/` 아래의 프로그램을 실행해 그 출력을 응답으로 보냅니다.
- GET: 쿼리 스트링을 `QUERY_STRING` 환경변수로 넘깁니다.
- POST: body를 표준 입력(stdin)으로 넘기고, 길이는 `CONTENT_LENGTH` 환경변수로 알려 줍니다 (body는 최대 8KB).
- 예시: `/cgi-bin/adder?num1=3&num2=16`

### 리버스 프록시
tiny 하나가 웹 서버와 리버스 프록시 역할을 함께 합니다. `routes.conf`의 규칙에 맞는 요청은 백엔드로 전달하고, 맞는 규칙이 없으면 tiny가 직접 응답합니다.

```
# prefix  strip(yes|no)  host:port [host:port ...]
/api      no     127.0.0.1:8001 127.0.0.1:8003
/calc/    yes    127.0.0.1:8001 127.0.0.1:8003
```
- **라우트 매칭**: 여러 규칙이 맞으면 prefix가 가장 긴 규칙을 씁니다. `/api` 규칙은 `/api/x`에는 적용되지만 `/apix`에는 적용되지 않습니다.
- **라운드로빈**: 한 규칙에 백엔드가 여러 개 있으면 요청을 차례로 나눠 보냅니다.
- **strip**: `yes`로 설정하면 prefix를 떼고 전달합니다 (`/calc/add` → `/add`).
- 백엔드로 전달할 때 hop-by-hop 헤더는 빼고, `X-Forwarded-For` 헤더를 추가합니다.

### 안정성
- **안전한 종료**: SIGINT나 SIGTERM을 받으면 열린 연결을 모두 닫고 종료합니다.
- **SIGPIPE 무시**: 클라이언트가 연결을 끊거나 CGI 프로그램이 먼저 종료돼도 서버는 계속 동작합니다.

### 에러 응답
| 코드 | 상황 |
|---|---|
| 400 | 헤더 형식이 잘못됨, body가 Content-Length보다 짧음 |
| 403 | 파일 읽기 권한이나 CGI 실행 권한이 없음 |
| 404 | 요청한 파일이 없음 |
| 405 | 정적 파일 경로에 POST 요청 |
| 411 | POST 요청에 Content-Length가 없음 |
| 413 | body가 8KB를 넘음 |
| 414 | 백엔드로 보낼 경로가 너무 김 |
| 501 | GET, POST 외의 메서드 |
| 502 | 백엔드에 연결하지 못했거나 백엔드가 응답 없이 연결을 닫음 |

### 데모 (브라우저로 확인하기)
`demo/`에 작은 카페 사이트("모퉁이 커피")가 있습니다. 정적 파일, CGI(POST), 리버스 프록시, 그리고 아래의 한계까지 브라우저에서 바로 확인할 수 있습니다.

```bash
bash demo/run-demo.sh     # 백엔드 :8001, :8002 + 프록시 :8000 실행, Ctrl+C로 모두 종료
```

| 주소 | 확인할 수 있는 것 |
|---|---|
| `http://localhost:8000/` | 정적 파일(html, css, js, png, jpg, gif), `/api/menu.txt`를 백엔드에서 받아 표시(라운드로빈) |
| `/cgi-bin/guestbook` | 방명록. 폼 POST를 CGI(bash)가 처리 |
| `/lab.html` | 서버 실험실. 블로킹 측정, 405·413·501·403·404, 라운드로빈을 버튼으로 확인 |

- 각 서버는 실행한 폴더를 웹 루트로 씁니다: 프록시는 `demo/site/`, 백엔드는 `demo/backend1/`, `demo/backend2/`.
- 방명록 글은 `demo/guestbook.db`, 백엔드 로그는 `demo/logs/`에 쌓입니다(git 제외).

### 한계

#### Keep-alive 미지원
- 요청 하나에 응답하면 바로 연결을 닫습니다. 응답 버전도 항상 `HTTP/1.0`입니다.
- 그래서 요청마다 TCP 연결을 새로 맺어야 하고(3-way handshake 반복), 파이프라이닝과 chunked 전송도 지원하지 않습니다.
- 백엔드로 전달할 때도 매번 새로 연결하고 `Connection: close`를 붙입니다. 백엔드 연결 풀은 없습니다.

#### 이벤트 루프 안의 블로킹 처리
select 기반 단일 프로세스지만, 아래 지점에서는 블로킹 호출을 하기 때문에 그동안 **다른 모든 연결이 멈춥니다**.

| 지점 | 블로킹 원인 | 영향 |
|---|---|---|
| 리버스 프록시 (`proxy_forward`) | 백엔드에 블로킹 `connect`, 응답이 끝날 때까지 `read` 반복 | 백엔드가 느리면 서버 전체가 그만큼 멈춤 |
| CGI (`serve_dynamic`) | `fork` 후 `wait`로 자식 종료를 기다림 | CGI 실행 시간만큼 다른 요청이 대기 |
| 응답 쓰기 (`Rio_writen`) | 다 쓸 때까지 반환하지 않음 | 수신이 느린 클라이언트에게 큰 파일을 보내는 동안 루프가 진행되지 않음 |
| 요청 읽기 (`rio_readlineb`) | 개행이 올 때까지 `read` 반복 | 한 줄이 중간까지만 도착하면 그 자리에서 대기. 상태 기계는 줄 경계에서만 다른 연결로 넘어감 |

- 연결별 idle 타임아웃이 없어서, 헤더를 아주 천천히 보내는 클라이언트(slowloris) 하나가 서버 전체를 멈출 수 있습니다.
- select의 `FD_SETSIZE`(보통 1024) 때문에 동시 연결 수에 상한이 있습니다.

#### 개선 방향
- 소켓을 non-blocking으로 바꾸고, 읽기·쓰기를 "가능한 만큼만" 하도록 상태 기계를 바이트 단위로 확장
- 백엔드 연결과 CGI 파이프도 이벤트 루프에 등록해서 응답을 비동기로 중계, `waitpid(WNOHANG)`/`SIGCHLD`로 자식 회수
- 연결별 마지막 활동 시각을 기록해 idle 타임아웃 적용
- HTTP/1.1 keep-alive: 응답 후 연결 상태를 `READ_REQLINE`으로 되돌려 재사용
- 연결 수가 많아지면 select 대신 `poll`/`epoll`

### 테스트
```bash
make test         # 파서 단위 테스트, CGI·서버·프록시 통합 테스트
make test-route   # 라우트 단위 테스트
```
