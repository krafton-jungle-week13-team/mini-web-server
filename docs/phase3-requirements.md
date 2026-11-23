# Phase 3 요구사항: POST 요청 처리

이 문서를 읽으면 Phase 3에서 Tiny 서버가 POST 요청을 받아 CGI 프로그램까지 body를 전달하려면 무엇을 구현해야 하는지 알 수 있어요.

## 요구사항

| ID | 분류 | 요구사항 | 비고 |
|---|---|---|---|
| REQ-POST-01 | 메서드 처리 | 서버는 GET과 POST 메서드를 인식하고 처리해야 한다. | |
| REQ-POST-02 | 메서드 처리 | 서버는 GET과 POST 외의 메서드로 요청을 받으면 501 Not Implemented로 응답해야 한다. | |
| REQ-POST-03 | 메서드 처리 | 서버는 정적 콘텐츠 경로로 POST 요청을 받으면 405 Method Not Allowed로 응답하고, 응답에 Allow: GET 헤더를 넣어야 한다. | |
| REQ-POST-04 | 헤더 파싱 | 서버는 요청 헤더에서 Host, Content-Length 값을 추출해 사용할 수 있어야 한다. | |
| REQ-POST-05 | 유효성 검사 | 서버는 POST 요청에 Content-Length 헤더가 없으면 411 Length Required로 응답해야 한다. | |
| REQ-POST-06 | 유효성 검사 | 서버는 POST 요청의 Content-Length 값이 8KB(8192바이트)를 넘으면 413 Content Too Large로 응답해야 한다. | |
| REQ-POST-07 | 요청 처리 | 서버는 POST 요청의 Content-Length에 적힌 바이트 수만큼 body를 읽어야 한다. | |
| REQ-POST-08 | CGI | 서버는 동적 요청을 처리할 때 CGI 프로그램에 REQUEST_METHOD 환경변수를 설정하고, POST 요청이면 CONTENT_LENGTH도 설정해야 한다. | |
| REQ-POST-09 | CGI | 서버는 동적 POST 요청의 body를 CGI 프로그램의 표준 입력(stdin)으로 전달해야 한다. | |
| REQ-POST-10 | CGI | CGI 프로그램(adder)은 POST 요청이면 CONTENT_LENGTH만큼 표준 입력(stdin)에서 body를 읽고, GET 요청이면 QUERY_STRING에서 인자를 읽어야 한다. | |
