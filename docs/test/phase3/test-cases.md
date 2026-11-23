# Phase 3 테스트 케이스: POST 요청 처리

이 문서를 읽으면 Phase 3 테스트 코드에서 어떤 입력을 넣고 어떤 결과를 확인해야 하는지 알 수 있어요.
케이스는 [Phase 3 요구사항](../../requirements/phase3/requirements.md)과 [Phase 3 설계](../../design/phase3/design.md)를 기준으로 해요.

## 테스트 구성

| 종류 | 대상 | 방법 |
| --- | --- | --- |
| 단위 테스트 | 헤더 파서 (`parse_content_length`, `trim`, `read_requesthdrs`) | C 테스트 코드에서 함수를 직접 호출 |
| 단위 테스트 | CGI 프로그램 (`adder`) | 환경변수와 stdin을 넣어 서버 없이 실행 |
| 기능 테스트 | 서버 전체 | 서버를 띄우고 `curl`, `nc`로 요청해 상태 코드 확인 |

### 전제: 헤더 파서 분리

헤더 파서 단위 테스트는 파서가 `http_parse.c`, `http_parse.h`로 분리돼 있다는 전제예요. 테스트 코드는 `http_parse.o`와 `csapp.o`만 링크해요.
분리 방법은 [설계 문서의 파일 분리](../../design/phase3/design.md#파일-분리)를 따라요.

## 1. 단위 테스트: 헤더 파서

### parse_content_length

`parse_content_length(v, &out)`는 성공하면 `0`을 돌려주고 `out`에 값을 넣어요. 실패하면 `-1`을 돌려줘요.

| ID | 입력 `v` | 기대 반환값 | 기대 `out` | 확인 내용 |
| --- | --- | --- | --- | --- |
| TC-CL-01 | `"14"` | `0` | `14` | 정상 값 |
| TC-CL-02 | `"0"` | `0` | `0` | 0 허용 |
| TC-CL-03 | `""` | `-1` | — | 빈 값 |
| TC-CL-04 | `"-1"` | `-1` | — | 음수 |
| TC-CL-05 | `"+14"` | `-1` | — | 부호 |
| TC-CL-06 | `" 14"` | `-1` | — | 앞 공백 (`trim` 후에 호출되므로 그대로 오면 오류) |
| TC-CL-07 | `"14abc"` | `-1` | — | 숫자 뒤 문자 |
| TC-CL-08 | `"99999999999999999999"` | `-1` | — | `long` 범위 초과 (ERANGE) |

### trim

`trim(s)`는 앞의 공백·탭과 뒤의 공백·탭·`\r`·`\n`을 지운 문자열의 시작 주소를 돌려줘요.

| ID | 입력 `s` | 기대 결과 | 확인 내용 |
| --- | --- | --- | --- |
| TC-TRIM-01 | `"localhost"` | `"localhost"` | 바꿀 것 없음 |
| TC-TRIM-02 | `"  localhost  "` | `"localhost"` | 앞뒤 공백 |
| TC-TRIM-03 | `"\tlocalhost\t"` | `"localhost"` | 앞뒤 탭 |
| TC-TRIM-04 | `" localhost\r\n"` | `"localhost"` | 줄 끝 `\r\n` |
| TC-TRIM-05 | `""` | `""` | 빈 문자열 |
| TC-TRIM-06 | `"  \r\n"` | `""` | 공백만 있음 |
| TC-TRIM-07 | `"local host"` | `"local host"` | 가운데 공백은 유지 |

### read_requesthdrs

`socketpair()`나 `pipe()`의 한쪽에 헤더 원문을 쓰고 닫은 뒤, 다른 쪽을 `rio_t`로 읽혀요.
`read_requesthdrs(rp, &h)`는 성공하면 `0`, 실패하면 `-1`을 돌려줘요. 실패는 `doit()`에서 400 Bad Request가 돼요.

| ID | 입력 헤더 원문 | 기대 반환값 | 기대 `h` | 확인 내용 |
| --- | --- | --- | --- | --- |
| TC-HDR-01 | `Host: localhost\r\nContent-Length: 14\r\n\r\n` | `0` | `host="localhost"`, `has_host=1`, `content_length=14`, `has_content_length=1` | 정상 헤더 |
| TC-HDR-02 | `host: localhost\r\ncontent-LENGTH: 14\r\n\r\n` | `0` | TC-HDR-01과 같음 | 이름 대소문자 무시 |
| TC-HDR-03 | `Host:   localhost  \r\n\r\n` | `0` | `host="localhost"` | 값 앞뒤 공백 제거 |
| TC-HDR-04 | `\r\n` | `0` | `has_host=0`, `has_content_length=0` | 헤더 없음 |
| TC-HDR-05 | `Accept: */*\r\n\r\n` | `0` | `has_host=0`, `has_content_length=0` | 모르는 헤더는 무시 |
| TC-HDR-06 | `Host localhost\r\n\r\n` | `-1` | — | 콜론 없음 |
| TC-HDR-07 | `: localhost\r\n\r\n` | `-1` | — | 이름이 비어 있음 |
| TC-HDR-08 | `Host : localhost\r\n\r\n` | `-1` | — | 이름과 콜론 사이 공백 |
| TC-HDR-09 | `Host: a\r\nHost: b\r\n\r\n` | `-1` | — | Host 중복 |
| TC-HDR-10 | `Content-Length: 14\r\nContent-Length: 20\r\n\r\n` | `-1` | — | 값이 다른 Content-Length 중복 |
| TC-HDR-11 | `Content-Length: 14\r\nContent-Length: 14\r\n\r\n` | `0` | `content_length=14` | 값이 같은 Content-Length 중복은 통과 |
| TC-HDR-12 | `Content-Length: abc\r\n\r\n` | `-1` | — | Content-Length 형식 오류 |
| TC-HDR-13 | `Host: localhost\r\n` (빈 줄 없이 EOF) | `-1` | — | 빈 줄 전에 연결 종료 |
| TC-HDR-14 | `X-Long: ` + `a` × `MAXLINE` + `\r\n\r\n` | `-1` | — | `MAXLINE`보다 긴 헤더 줄 |

## 2. 단위 테스트: adder 단독

adder는 서버 없이 환경변수와 stdin으로 실행해요.

```bash
printf 'num1=3&num2=16' | REQUEST_METHOD=POST CONTENT_LENGTH=14 ./cgi-bin/adder
QUERY_STRING='num1=3&num2=16' REQUEST_METHOD=GET ./cgi-bin/adder
```

모든 케이스에서 adder는 종료 코드 `0`으로 끝나야 하고, 시그널로 죽으면 안 돼요.
에러 케이스는 본문에 에러 메시지(예: `Invalid arguments`)가 나오는지 확인해요.

| ID | `REQUEST_METHOD` | `CONTENT_LENGTH` | `QUERY_STRING` | stdin | 기대 결과 | 요구사항 |
| --- | --- | --- | --- | --- | --- | --- |
| TC-ADD-01 | `POST` | `14` | — | `num1=3&num2=16` | `3 + 16 = 19` | REQ-POST-10 |
| TC-ADD-02 | `GET` | — | `num1=3&num2=16` | — | `3 + 16 = 19` | REQ-POST-10 |
| TC-ADD-03 | `POST` | 설정 안 함 | — | `num1=3&num2=16` | 에러 메시지 | REQ-POST-10 |
| TC-ADD-04 | `POST` | `abc` | — | `num1=3&num2=16` | 에러 메시지 | REQ-POST-10 |
| TC-ADD-05 | `POST` | `-1` | — | `num1=3&num2=16` | 에러 메시지 | REQ-POST-10 |
| TC-ADD-06 | `POST` | `8193` | — | `num1=3&num2=16` | 에러 메시지 | REQ-POST-10 |
| TC-ADD-07 | `POST` | `20` | — | `num1=3&num2=16` (14바이트) | 에러 메시지 | REQ-POST-10 |
| TC-ADD-08 | `POST` | `5` | — | `hello` | 에러 메시지 | REQ-POST-10 |
| TC-ADD-09 | `POST` | `6` | — | `num1=3` | 에러 메시지 | REQ-POST-10 |
| TC-ADD-10 | `GET` | — | `""` (빈 문자열) | — | 에러 메시지 | REQ-POST-10 |

## 3. 기능 테스트: 서버 + curl/nc

서버를 `./tiny 8000`으로 띄우고 요청을 보내요. 상태 코드는 `curl -s -o /dev/null -w '%{http_code}'`로 확인해요.
`Content-Length`를 빼거나 틀리게 보내야 하는 케이스는 `printf '...' | nc -N localhost 8000`으로 요청 원문을 보내요.

| ID | 요청 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| TC-FN-01 | `POST /cgi-bin/adder`, body `num1=3&num2=16` | 200, 본문에 `3 + 16 = 19` | REQ-POST-01, 07, 08, 09 |
| TC-FN-02 | `GET /cgi-bin/adder?num1=3&num2=16` | 200, 본문에 `3 + 16 = 19` | REQ-POST-01, 10 |
| TC-FN-03 | `PUT /cgi-bin/adder` | 501 | REQ-POST-02 |
| TC-FN-04 | `DELETE /cgi-bin/adder` | 501 | REQ-POST-02 |
| TC-FN-05 | `get /home.html` (소문자 메서드) | 200 | REQ-POST-01 |
| TC-FN-05-1 | `post /cgi-bin/adder`, body `num1=3&num2=16` (소문자 메서드) | 200, 본문에 `3 + 16 = 19` | REQ-POST-01, 09 |
| TC-FN-06 | `POST /home.html`, body `num1=3&num2=16` | 405, 헤더에 `Allow: GET` | REQ-POST-03 |
| TC-FN-07 | `POST /cgi-bin/adder`, `Content-Length` 없음 | 411 | REQ-POST-05 |
| TC-FN-08 | `POST /cgi-bin/adder`, body 8192바이트 | 200 (adder 응답) | REQ-POST-06 |
| TC-FN-09 | `POST /cgi-bin/adder`, body 8193바이트 | 413 | REQ-POST-06 |
| TC-FN-10 | `POST /cgi-bin/adder`, `Content-Length: 20`, body 14바이트 후 연결 종료 | 400 | REQ-POST-07 |
| TC-FN-11 | `POST /cgi-bin/adder`, `Content-Length: 0` | 200, adder 에러 메시지. 다음 요청도 정상 처리 | REQ-POST-07 |
