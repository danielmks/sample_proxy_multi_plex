#ifndef PROXY_H
#define PROXY_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define BUFFER_SIZE         4096
#define LISTEN_BACKLOG      10
#define DEFAULT_HTTP_PORT   80
#define DEFAULT_HTTPS_PORT  443
#define DEFAULT_SERVER_PORT 8888
#define MAX_BLOCKED_DOMAINS 100

// 차단할 도메인 목록 관련 전역 변수
extern char *blocked_domains[MAX_BLOCKED_DOMAINS];
extern int blocked_count;

// epoll에서 사용할 fd의 종류
typedef enum {
    FD_CLIENT,
    FD_REMOTE
} fd_type_t;

// 전후 연결(context) 정보를 담은 구조체
struct connection;  // 전방 선언

typedef struct fd_context {
    struct connection *conn;
    fd_type_t type;   // FD_CLIENT 또는 FD_REMOTE
} fd_context;

// 클라이언트와 원격 서버 간 연결 상태 정보
struct connection {
    int client_fd;     // 클라이언트 소켓
    int remote_fd;     // 원격 서버 소켓 (초기 요청 후 생성)
    int initialized;   // 0: 초기 요청 미처리, 1: 처리 완료
    int is_https;      // 1: HTTPS (CONNECT 방식), 0: HTTP
    fd_context *client_ctx; // 클라이언트 이벤트 context
    fd_context *remote_ctx; // 원격 서버 이벤트 context
};

// 함수 프로토타입
void load_blocked_domains(const char *filename);
int is_blocked_domain(const char *host);
void send_blocked_response(int client_socket);
int set_nonblocking(int fd);
void run_proxy_server(int listen_socket);

#endif // PROXY_H

