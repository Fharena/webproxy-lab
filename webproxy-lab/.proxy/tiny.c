/* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *     GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */
#include "csapp.h"
#include "stdio.h"

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize, char *version);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs, char *version);
void clienterror(char *version, int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

int main(int argc, char **argv) {
  int listenfd, connfd;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;

  /* Check command line args */
  if (argc != 2) { 
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }
  
  listenfd = Open_listenfd(argv[1]); //듣기 소켓 오픈
  while (1) {//
    clientlen = sizeof(clientaddr);
    //accept함수는 블로킹 상태로 들어가 커널에서 신호받을때까지 대기하게 함. 연결요청 이후 connfd 반환
    connfd = Accept(listenfd, (SA *)&clientaddr,&clientlen);  // line:netp:tiny:accept
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE,0);//호스트 이름과, 포트번호 반환
    printf("Accepted connection from (%s, %s)\n", hostname, port);
    // 
    doit(connfd);   // line:netp:tiny:doit
    Close(connfd);  // line:netp:tiny:close
  }
}

//client의 http요청을 처리하는 함수
void doit(int fd)
{
  int is_static;
  struct stat sbuf;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char filename[MAXLINE], cgiargs[MAXLINE];
  rio_t rio;

   /* Read request line and headers */
  Rio_readinitb(&rio, fd); //읽기전 초기화, fd의 내용물을 rio로 옮김.
  Rio_readlineb(&rio, buf, MAXLINE); //rio 내용 한줄씩 buf로 읽음(최대 MAXLINE까지 /n 만나면 멈춤) -> 현재는 요청라인 읽는용
  printf("Request headers:\n");
  printf("%s", buf);
  sscanf(buf, "%s %s %s", method, uri, version);

  


  if (strcasecmp(method, "GET")){//GET만 지원함, GET이 아니면 에러메세지 출력하고 return
    clienterror(version, fd, method, "501", "Not Implemented",
                "Tiny does not implement this method");
    return;
  }
  read_requesthdrs(&rio);//요청 헤더 읽기, 한줄씩 읽고 출력함

  /* Parse URI from GET request */
  is_static = parse_uri(uri, filename, cgiargs);//정적이면 1, 동적이면 0 반환
  if (stat(filename, &sbuf) < 0){//파일 존재여부 확인
    clienterror(version,fd, filename, "404", "Not found",
                "Tiny couldn't find this file");
    return;
  }

  if (is_static)
  { /* Serve static content */

    //일반파일이 아니거나, 파일 읽기권한이 없는 경우 403 에러 안내 실행
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode))
    //S_ISREG(sbuf.st_mode) 정규 파일(일반적인 텍스트, 이미지, 실행 파일 등 인지 확인),디렉토리, 소켓, 디바이스 파일이면 false
    //S_IRUSR & sbuf.st_mode 이 파일에 **owner(소유자)**가 읽기 권한을 갖고 있는지 확인,S_IRUSR는 상수 값 (0400)이고, 소유자 읽기 비트,sbuf.st_mode는 파일 권한 비트
    {
      clienterror(version,fd, filename, "403", "Forbidden",
                  "Tiny couldn't read the file");
      return;
    }
    //일반파일, 읽기권한이 있다면, serve_static 실행
    serve_static(fd, filename, sbuf.st_size,version);
  }
  //동적 컨텐츠를 요청 받았다면, 
  else
  { /* Serve dynamic content */
    // S_IXUSR는 사용자(user)에 대한 실행 권한을 나타내는 상수
    // st_mode는 stat 구조체에서 파일의 모드를 나타내는 필드
    // 일반 파일이 아니거나, 실행가능한 파일이 아니거나, 실행 권한이 없는 경우 403에러 안내실행
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode))
    {
      clienterror(version,fd, filename, "403", "Forbidden",
                  "Tiny couldn't run the CGI program");
      return;
    }

    serve_dynamic(fd, filename, cgiargs,version);//fd로 파일 출력.
  }
}

// HTTP 클라이언트에게 오류 응답을 보내기 위한 함수.
void clienterror(char *version,int fd, char *cause, char *errnum, char *shortmsg,char *longmsg){
  char buf[MAXLINE], body[MAXBUF];

  /* Build the HTTP response body */
  sprintf(body, "<html><title>Tiny Error</title>");
  sprintf(body, "%s<body bgcolor=""ffffff"">\r\n",body);
  sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
  sprintf(body, "%s<p>%s: %s</p>\r\n", body, longmsg, cause);
  sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body); 

  /* Print the HTTP response */
  sprintf(buf, "%s %s %s\r\n",version , errnum, shortmsg);
  // 
  //
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-type: text/html\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
  Rio_writen(fd, buf, strlen(buf));
  Rio_writen(fd, body, strlen(body));
}



void read_requesthdrs(rio_t *rp)
{
  char buf[MAXLINE];

  Rio_readlineb(rp, buf, MAXLINE);
  while (strcmp(buf, "\r\n"))
  {
    Rio_readlineb(rp, buf, MAXLINE);//힌줄씩 읽고
    printf("%s", buf);//출력
  }
  return;
}

int parse_uri(char *uri, char *filename, char *cgiargs){
  char *ptr;

  if (!strstr(uri, "cgi-bin"))
  { /* Static content */
    strcpy(cgiargs, ""); // CGI 인지 없음
    strcpy(filename, "."); // 현재 디렉토리 기준
    strcat(filename, uri); // URI 이어붙이기
    if (uri[strlen(uri) - 1] == '/') // 디렉토리로 끝나면 기본페이지 붙여주기.
      strcat(filename, "home.html");
    return 1;
  }
  else
  { /* Dynamic content */
    ptr = index(uri, '?'); // ?위치 찾기
    if (ptr)
    {
      strcpy(cgiargs, ptr + 1);// ?보다 한칸뒤부터 복사 -> 인자 복사
      *ptr = '\0'; //? 위치에 \0 넣어주기
    }
    else//? 없으면 인자 없다는뜻.
      strcpy(cgiargs, "");//인자 비우고
    strcpy(filename, ".");//파일이름에 .붙이고
    strcat(filename, uri); // URI 이어붙이기
    return 0;
  }
}

void serve_static(int fd, char *filename, int filesize, char *version)
{
  int srcfd;
  char *srcp, filetype[MAXLINE], buf[MAXBUF];
  rio_t rio;

  /* Send response headers to client */
  get_filetype(filename, filetype);//파일의 타입 얻어오고
  sprintf(buf, "%s 200 OK\r\n",version);
  sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);//헤더쪽 줄바꿈은 \r\n이 관례라 함
  sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
  sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);
  Rio_writen(fd, buf, strlen(buf));
  printf("Response headers:\n");
  printf("%s", buf);

  /* Send response body to client */
  srcfd = Open(filename, O_RDONLY, 0);//파일 열기
  // srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0); //파일을 메모리에 매핑
  
    // 읽기 위한 rio 초기화
    Rio_readinitb(&rio, srcfd);

    // 파일 크기만큼 동적 할당
    srcp = (char *)malloc(filesize);
    if (!srcp) {
        fprintf(stderr, "malloc error\n");
        Close(srcfd);
        return;
    }

    // 파일 전체 읽기
    if (Rio_readn(srcfd, srcp, filesize) != filesize) {
        fprintf(stderr, "rio_readn error\n");
        free(srcp);
        Close(srcfd);
        return;
    }

  Close(srcfd);//쓴파일 닫기
  Rio_writen(fd, srcp, filesize);//fd에 srcp 내용 filesize만큼 쓰기
  // Munmap(srcp, filesize);//메모리 파일 정리 및 반납
}

void get_filetype(char *filename, char *filetype)
{
  if (strstr(filename, ".html"))
    strcpy(filetype, "text/html");
  else if (strstr(filename, ".gif"))
    strcpy(filetype, "image/gif");
  else if (strstr(filename, ".jpg"))
    strcpy(filetype, "image/jpeg");
  else if (strstr(filename, ".png"))
    strcpy(filetype, "image/png");
  else if (strstr(filename, ".js"))
    strcpy(filetype, "application/javascript");
  else if (strstr(filename, ".ico"))
    strcpy(filetype, "image/x-icon");
  else if (strstr(filename, ".mpg"))
    strcpy(filetype, "video/mpeg");
    else if (strstr(filename, ".mp4"))
    strcpy(filetype, "video/mp4");
  else
    strcpy(filetype, "text/plain");
  
}

void serve_dynamic(int fd, char *filename, char *cgiargs, char *version)
{
  char buf[MAXLINE], *emptylist[] = {NULL};

  /* Return first part of HTTP response */
  sprintf(buf, "%s 200 OK\r\n", version);
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Server: Tiny Web Server\r\n\r\n");
  Rio_writen(fd, buf, strlen(buf));

  if (Fork() == 0)//자식 만들고
  { /* child */
    /* Real server would set all CGI vars here */
    setenv("QUERY_STRING", cgiargs, 1); // 환경변수 인자로 설정. 마지막 인자는 0,1이 들어가며 기존값이 있을때 덮어쓰면 1, 안덮어쓰면 0
    Dup2(fd, STDOUT_FILENO);              //fd를 stdout으로 재지정.STDOUT_FILENO == 1 과 같음., 자동으로 1 close 해줌
    Execve(filename, emptylist, environ); /* Run CGI program */
  }
  Wait(NULL); /* Parent waits for and reaps child */
}