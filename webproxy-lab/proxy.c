#include <stdio.h>
#include <pthread.h> 
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define CACHE_BLOCKS (MAX_CACHE_SIZE / MAX_OBJECT_SIZE) // 10개임
void handle_sigint(int sig) {// 키보드 인터럽트에 대응할 시그널 햄들러 함수
    cache_cleanup();   // 락/메모리 해제
    exit(0);           // 정상 종료
}
/* 캐시 블록: 개별 웹 객체 */
typedef struct {
    pthread_rwlock_t lock;       // 블록 락
    char uri[MAXLINE];           // 키
    char *object;                // 데이터 (malloc 사용)
    int size;                    // 데이터 크기
    unsigned long last_used;     // LRU 갱신용
} cache_block;

/* 전체 캐시 구조체 */
typedef struct {
    cache_block blocks[CACHE_BLOCKS];
    int used_size;                // 총 사용된 바이트
    pthread_rwlock_t lock;        // 전체 캐시 락 (LRU, 블록 교체 등)
} cache_t;

/* 전역 캐시 */
cache_t cache;

void *thread(void *vargp);
void doit(int connfd);
void cache_init();
int cache_find(char *uri, char *buf, int *size_out);
void cache_evict_if_needed(int new_object_size);
void cache_insert(char *uri, char *object, int size);
void cache_cleanup();
/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

    int main(int argc, char **argv) {
      int listenfd, *connfdp;
      socklen_t clientlen;
      struct sockaddr_storage clientaddr;
      char hostname[MAXLINE], port[MAXLINE];
      pthread_t tid;

      signal(SIGINT, handle_sigint);
  
      if (argc != 2) {
          fprintf(stderr, "usage: %s <port>\n", argv[0]);
          exit(1);
      }
      //전체 캐시 초기화
      cache_init();

    //클라이언트로터 들을 소켓 오픈
      listenfd = Open_listenfd(argv[1]);//듣기 소켓 오픈
  
      while (1) {
          clientlen = sizeof(clientaddr);
          //accept함수는 블로킹 상태로 들어가 커널에서 신호받을때까지 대기하게 함. 연결요청 이후 connfd 반환
          //accept하면서 클라이언트로 응답 돌려줄 fd 만들고.
          connfdp = malloc(sizeof(int)); // 루프돌때마다 메모리 할당해서 서로 간섭 안하게 하기.
          *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);
          Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);//클라이언트의 호스트 이름과, 포트번호 반환
          printf("Accepted connection from (%s, %s)\n", hostname, port);
          Pthread_create(&tid, NULL,thread,connfdp);
  
        //   doit(connfd); // 요청 처리
        //   Close(connfd);
      }
  
      return 0;
  }
void *thread(void *vargp){
    int connfd = *((int*)vargp);
    pthread_detach(pthread_self());//지 할꺼 끝나면 알아서 종료
    free(vargp);
    doit(connfd);
    close(connfd);
    return NULL;
}

void doit(int connfd) {
    int proxytoendfd, n;
    char line[MAXLINE], request_buf[MAXLINE * 10] = "", fromendbuf[MAXLINE];
    char method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char path[MAXLINE], headertype[MAXLINE];
    char endhostname[MAXLINE], endport[MAXLINE];
    char cache_buf[MAX_OBJECT_SIZE];//캐시용 버퍼
    int total_size = 0;
    rio_t rio, endrio;
    //   printf("요청 처리 시작\n"); // 병렬 테스트용 로그
    //   sleep(3);
    //   printf("요청 처리 완료\n");
    Rio_readinitb(&rio, connfd);//초기화해주고
    //클라이언트에서 헤더 받고 서버로 보내기.
    // 헤더에서 "Host" 줄 찾기
    //1. 요청라인 재구성
    // 요청 라인 읽기 및 파싱
    Rio_readlineb(&rio, line, MAXLINE);
    sscanf(line, "%s %s %s", method, uri, version);


    // 캐시 확인 -> hit시 클라이언트로 바로 전송하고 아니면 계속 진행.
    int cached_size;
    if (cache_find(uri, cache_buf, &cached_size)) {//cache find는 cache hit시에 1반환. -> if내부로 진입.
        Rio_writen(connfd, cache_buf, cached_size); // 바로 클라이언트로 전송.
        return; // 캐시 hit → 바로 전송 후 종료
    }



  // 전체 URL에서 path만 추출 -> 브라우저는 proxy의 경우 앞에 http:// 붙은 풀사이즈 경로로 요청줌
  if (strstr(uri, "http://") == uri) {
      char *start = strchr(uri + 7, '/');// "http://" 건너뛰고 '/'부터 찾기
      if (start)
          strcpy(path, start);// 예: "/home.html"
      else
          strcpy(path, "/");
  } else {//이미 경로만 들어온 경우 ex.http 1.0기반 실행
      strcpy(path, uri);
  }
//요청라인 재구성-> 강제로 http1.0 으로
  sprintf(request_buf, "%s %s HTTP/1.0\r\n", method, path);

  // 헤더 읽기
  while (Rio_readlineb(&rio, line, MAXLINE) > 0 && strcmp(line, "\r\n") != 0) {
      strcat(request_buf, line);

      if (strncasecmp(line, "Host:", 5) == 0) {
          sscanf(line + 5, "%s", endhostname);
          char *colon = strchr(endhostname, ':');
          if (colon) {
              *colon = '\0';
              strcpy(endport, colon + 1);
          } else {
              strcpy(endport, "80");//포트 안들어있으면 기본포트 세팅(실습은 로컬이라 상관없고 기본적으로 http 규약이라 넣음)
          }
      }
  }
  strcat(request_buf, user_agent_hdr); 
  strcat(request_buf, "Connection: close\r\n");
  strcat(request_buf, "\r\n");// 헤더 끝 표시

    //아래는 서버응답을 클라이언트로 보내주기.
  proxytoendfd = open_clientfd(endhostname, endport);// proxy to endserver용 소켓 오픈.
  if (proxytoendfd < 0) {//연결실패
      fprintf(stderr, "Failed to connect to end server %s:%s\n", endhostname, endport);
      return;
  }

  Rio_writen(proxytoendfd, request_buf, strlen(request_buf));//위에서 클라이언트로 부터 받은 요청내용 엔드서버로 보내주기
  Rio_readinitb(&endrio, proxytoendfd);//초기화해주고

  while (Rio_readlineb(&endrio, fromendbuf, MAXLINE) > 0) {// Rio_readlineb--> ㅅㅂ 이거도 블로킹 함수구나?
    Rio_writen(connfd, fromendbuf, strlen(fromendbuf));
    if (total_size + strlen(fromendbuf) <= MAX_OBJECT_SIZE)//적은 size만큼 뒤로 가면서 저장.
        memcpy(cache_buf + total_size, fromendbuf, strlen(fromendbuf));//캐시에도 헤더 저장 -- > 캐시버퍼 맨앞에 저장.
    total_size += strlen(fromendbuf);//보낸 fromendbuf사이즈만큼 캐시  사이즈 누적.
    if (strcmp(fromendbuf, "\r\n") == 0)
        break;
  }

  while ((n = Rio_readn(proxytoendfd, fromendbuf, MAXLINE)) > 0) {
      Rio_writen(connfd, fromendbuf, n);

        // 캐시에 넣을 수 있으면 복사
        if (total_size + n <= MAX_OBJECT_SIZE)
            memcpy(cache_buf + total_size, fromendbuf, n);//total_size에 미리 헤더 사이즈 체크 해놨으니까 그 이후로 포인터 이동해서 memcpy.
        total_size += n;//내용물 전체 사이즈 완성.
    }

    // 응답 전체가 캐시에 들어갈 수 있으면 저장
    if (total_size <= MAX_OBJECT_SIZE) {//토탈 사이즈가 max 옵젝 사이즈보다 작거나 같은지 체크.
    cache_insert(uri, cache_buf, total_size);
  }

  

  Close(proxytoendfd);
}

void cache_init() { //  캐시 구조체->블록 구조체 초기화.
    pthread_rwlock_init(&cache.lock, NULL); // 캐시 구조체의 lock을 NULL(기본속성)로 초기화 --> 기본속성은 동일 프로세스내의 스레드끼리만 락 공유하는 private 속성임.
    cache.used_size = 0; // 사용중인 캐시의 사이즈 0으로 초기화

    for (int i = 0; i < CACHE_BLOCKS; i++) { // 10개의 블록 초기화.
        pthread_rwlock_init(&cache.blocks[i].lock, NULL);
        cache.blocks[i].object = NULL;
        cache.blocks[i].size = 0;
        cache.blocks[i].last_used = 0;
        cache.blocks[i].uri[0] = '\0';
    }
}

int cache_find(char *uri, char *buf, int *size_out) { //캐시 탐색
    for (int i = 0; i < CACHE_BLOCKS; i++) { //블록 갯수만큼 루프
        pthread_rwlock_rdlock(&cache.blocks[i].lock);//해당 캐시 블록의 읽기락을 획득. -> 쓰기중인 쓰레드가 있을수도 있어서 무결성을 위해 
        //락을 설정해두었고, 읽기락으로 획득해서 정보 읽는것.
        if (strcmp(cache.blocks[i].uri, uri) == 0) { //strcmp==0일때->요청으로 온 uri와 캐시에 있는 uri가 같을때 -> 캐시 hit
            memcpy(buf, cache.blocks[i].object, cache.blocks[i].size); //버퍼로 해당 블록의 오브젝트를 사이즈만큼 memcpy(메모리영역 카피)
            *size_out = cache.blocks[i].size; //함수 밖으로 size 돌려주기 위한곳.
            cache.blocks[i].last_used = (unsigned long)time(NULL);//사용시간 찍어주고
            pthread_rwlock_unlock(&cache.blocks[i].lock);//언락.
            return 1; // cache hit
        }
        pthread_rwlock_unlock(&cache.blocks[i].lock);
    }
    return 0; // cache miss
}

void cache_evict_if_needed(int new_object_size) {//꽉차있으면 자리 뺐어야지.->LRU 알고리즘.
    while (cache.used_size + new_object_size > MAX_CACHE_SIZE) { // 현재 사용중인 캐시 사이즈+새로 넣을 내용물 사이즈가 MAX보다 크면-> 자리 없어요.->루프 진입.
        int lru_index = -1;//LRU 인덱스-> 가장 사용한지 오래된 블록의 인덱스 체크용.
        unsigned long min_time = (unsigned long)-1; // -1는 부호없는 자료형으로 바꾸면 2의보수의 2의보수가 되므로 최댓값으로 바뀜.

        for (int i = 0; i < CACHE_BLOCKS; i++) {//캐시블록 뒤지는 루프
            pthread_rwlock_rdlock(&cache.blocks[i].lock);//캐시블록의 읽기 락 획득.
            if (cache.blocks[i].object != NULL && cache.blocks[i].last_used < min_time) {//최솟값찾기.
                min_time = cache.blocks[i].last_used;
                lru_index = i;//최솟값 인덱스 저장.
            }
            pthread_rwlock_unlock(&cache.blocks[i].lock);//언락.
        }

        if (lru_index == -1) return;//캐시에 유효한 블록이 없음 (비어 있음)

        pthread_rwlock_wrlock(&cache.blocks[lru_index].lock);//캐시블록의 쓰기락 획득(캐시 구조체의 락은 insert, 즉 외부함수에서 선언함.)
        free(cache.blocks[lru_index].object);//object free 해주고
        cache.used_size -= cache.blocks[lru_index].size;//캐시 전체 사이즈를 비우는 캐시블록 사이즈만큼 -=
        cache.blocks[lru_index].object = NULL;//다른 요소들도 전부 초기화.
        cache.blocks[lru_index].size = 0;
        cache.blocks[lru_index].uri[0] = '\0';
        pthread_rwlock_unlock(&cache.blocks[lru_index].lock);//다 썼으니까 쓰기락 언락.
    }
}

void cache_insert(char *uri, char *object, int size) {//캐시에 입력.-> 입력전에 cache_evict_if_needed(size); 함수 실행해서 자리 있는지 확인하고 없으면 자리 만들어주기.
    if (size > MAX_OBJECT_SIZE) return; // 최대 오브젝트 사이즈 넘으면 캐싱 안함.

    pthread_rwlock_wrlock(&cache.lock); // 전체 캐시 보호 (used_size 변경, LRU 계산)
    cache_evict_if_needed(size); // 캐시 자리 뺏기.

    for (int i = 0; i < CACHE_BLOCKS; i++) {// 블록 순회
        pthread_rwlock_wrlock(&cache.blocks[i].lock);//캐시블록 쓰기락 획득.
        if (cache.blocks[i].object == NULL) {//해당 블록 비었으면 그 블록에 캐싱해줄거임.
            cache.blocks[i].object = malloc(size); // 동적메모리 할당 해주고.
            memcpy(cache.blocks[i].object, object, size);//버퍼째로 넘겨준  object 내용물 그대로 복사
            strcpy(cache.blocks[i].uri, uri);//넘어온 uri로 갈아주고.(path 쓰면 같은 경로는 캐싱 안하겠지만 진짜 proxy라고 생각하면 외부 ip의 같은 경로까지 생각해야하므로 uri사용.)
            cache.blocks[i].size = size;//사이즈 바꿔주고.
            cache.blocks[i].last_used = (unsigned long)time(NULL);//(Unix epoch 이후 초,인자가 NULL이면 초단위로 정수를 반환함.
            cache.used_size += size; //총 캐시 사이즈 올려주고
            pthread_rwlock_unlock(&cache.blocks[i].lock);// 캐시 블록의 쓰기락 언락.
            break;
        }
        else pthread_rwlock_unlock(&cache.blocks[i].lock);// if 안들어간 경우에 언락.
    }

    pthread_rwlock_unlock(&cache.lock);//캐시구조체 쓰기락 언락.
}

void cache_cleanup() {//리소스 반환을 위한 함수. -> 키보드 인터럽트로 종료하니까 시그널 핸들러로 넣어두면 됨.
    pthread_rwlock_destroy(&cache.lock);
    for (int i = 0; i < CACHE_BLOCKS; i++) {
        pthread_rwlock_destroy(&cache.blocks[i].lock);
        if (cache.blocks[i].object != NULL) {
            free(cache.blocks[i].object);
        }
    }
}
