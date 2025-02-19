#include "proxy.h"
#include <sys/epoll.h>

char *blocked_domains[MAX_BLOCKED_DOMAINS];
int blocked_count = 0;

// 소켓을 논블로킹 모드로 전환하는 헬퍼 함수
int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// blocked.txt 파일로부터 차단 도메인 목록 로드
void load_blocked_domains(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        perror("blocked.txt 파일 열기 실패");
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strlen(line) > 0 && blocked_count < MAX_BLOCKED_DOMAINS) {
            blocked_domains[blocked_count++] = strdup(line);
        }
    }
    fclose(fp);
}

// 대소문자 구분 없이 차단 도메인 체크
int is_blocked_domain(const char *host) {
    for (int i = 0; i < blocked_count; i++) {
        if (strcasecmp(host, blocked_domains[i]) == 0)
            return 1;
    }
    return 0;
}

// 로컬 파일 403message.html의 내용을 읽어 클라이언트에 전송
void send_blocked_response(int client_socket) {
    FILE *fp = fopen("403message.html", "r");
    if (!fp) {
        perror("403message.html 파일 열기 실패");
        return;
    }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *content = malloc(fsize + 1);
    if (!content) {
        perror("메모리 할당 실패");
        fclose(fp);
        return;
    }
    fread(content, 1, fsize, fp);
    content[fsize] = '\0';
    fclose(fp);
    char header[512];
    snprintf(header, sizeof(header),
             "HTTP/1.1 403 Forbidden\r\n"
             "Content-Type: text/html\r\n"
             "Content-Length: %ld\r\n"
             "\r\n", fsize);
    write(client_socket, header, strlen(header));
    write(client_socket, content, fsize);
    free(content);
}

// 연결 종료 시 모든 리소스 해제
static void close_connection(fd_context *ctx) {
    if (!ctx || !ctx->conn)
        return;
    struct connection *conn = ctx->conn;
    if (conn->client_fd >= 0)
        close(conn->client_fd);
    if (conn->remote_fd >= 0)
        close(conn->remote_fd);
    if (conn->client_ctx) {
        free(conn->client_ctx);
        conn->client_ctx = NULL;
    }
    if (conn->remote_ctx) {
        free(conn->remote_ctx);
        conn->remote_ctx = NULL;
    }
    free(conn);
}

// 클라이언트로부터 초기 요청을 읽어 원격 서버 연결을 설정하는 함수
static int process_initial_request(struct connection *conn) {
    char buffer[BUFFER_SIZE];
    int n = recv(conn->client_fd, buffer, BUFFER_SIZE - 1, 0);
    if (n <= 0)
        return -1;
    buffer[n] = '\0';

    // HTTPS CONNECT 요청인지 확인
    if (strncmp(buffer, "CONNECT", 7) == 0) {
        conn->is_https = 1;
        char target[256];
        int port = DEFAULT_HTTPS_PORT;
        char *p = buffer + 8;
        char *end = strstr(p, " ");
        if (!end)
            return -1;
        *end = '\0';
        char *colon = strchr(p, ':');
        if (colon) {
            *colon = '\0';
            strncpy(target, p, sizeof(target) - 1);
            target[sizeof(target) - 1] = '\0';
            port = atoi(colon + 1);
        } else {
            strncpy(target, p, sizeof(target) - 1);
            target[sizeof(target) - 1] = '\0';
        }
        printf("HTTPS 요청: %s:%d\n", target, port);
        if (is_blocked_domain(target)) {
            printf("차단된 도메인: %s\n", target);
            send_blocked_response(conn->client_fd);
            return -1;
        }
        struct hostent *he = gethostbyname(target);
        if (!he)
            return -1;
        conn->remote_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (conn->remote_fd < 0)
            return -1;
        struct sockaddr_in remote_addr;
        memset(&remote_addr, 0, sizeof(remote_addr));
        remote_addr.sin_family = AF_INET;
        remote_addr.sin_port = htons(port);
        memcpy(&remote_addr.sin_addr, he->h_addr_list[0], he->h_length);
        if (connect(conn->remote_fd, (struct sockaddr*)&remote_addr, sizeof(remote_addr)) < 0) {
            perror("connect");
            return -1;
        }
        const char *established = "HTTP/1.1 200 Connection Established\r\n\r\n";
        if (write(conn->client_fd, established, strlen(established)) < 0)
            return -1;
    } else {  // 일반 HTTP 요청
        conn->is_https = 0;
        char *host_header = strstr(buffer, "Host:");
        if (!host_header)
            return -1;
        host_header += 5;
        while (*host_header == ' ' || *host_header == '\t')
            host_header++;
        char host[256];
        int i = 0;
        while (*host_header != '\r' && *host_header != '\n' &&
               *host_header != '\0' && i < (int)(sizeof(host) - 1)) {
            host[i++] = *host_header++;
        }
        host[i] = '\0';
        printf("HTTP 요청: %s\n", host);
        if (is_blocked_domain(host)) {
            printf("차단된 도메인: %s\n", host);
            send_blocked_response(conn->client_fd);
            return -1;
        }
        int port = DEFAULT_HTTP_PORT;
        struct hostent *he = gethostbyname(host);
        if (!he)
            return -1;
        conn->remote_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (conn->remote_fd < 0)
            return -1;
        struct sockaddr_in remote_addr;
        memset(&remote_addr, 0, sizeof(remote_addr));
        remote_addr.sin_family = AF_INET;
        remote_addr.sin_port = htons(port);
        memcpy(&remote_addr.sin_addr, he->h_addr_list[0], he->h_length);
        if (connect(conn->remote_fd, (struct sockaddr*)&remote_addr, sizeof(remote_addr)) < 0) {
            perror("connect");
            return -1;
        }
        if (write(conn->remote_fd, buffer, n) < 0)
            return -1;
    }
    // 원격 서버 소켓도 논블로킹으로 설정
    set_nonblocking(conn->remote_fd);
    return 0;
}

// epoll을 사용한 프록시 서버의 메인 이벤트 루프
void run_proxy_server(int listen_socket) {
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return;
    }

    // listen 소켓은 별도 fd(정수)로 epoll에 등록
    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = listen_socket;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_socket, &event) < 0) {
        perror("epoll_ctl listen_socket");
        close(epoll_fd);
        return;
    }

    struct epoll_event events[64];
    while (1) {
        int n_fds = epoll_wait(epoll_fd, events, 64, -1);
        if (n_fds < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n_fds; i++) {
            // listen 소켓 이벤트 처리
            if (events[i].data.fd == listen_socket) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(listen_socket, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd < 0) {
                    perror("accept");
                    continue;
                }
                set_nonblocking(client_fd);
                // 새 연결 생성
                struct connection *conn = malloc(sizeof(struct connection));
                if (!conn) {
                    close(client_fd);
                    continue;
                }
                conn->client_fd = client_fd;
                conn->remote_fd = -1;
                conn->initialized = 0;
                conn->is_https = 0;
                conn->client_ctx = NULL;
                conn->remote_ctx = NULL;
                // 클라이언트 이벤트 context 할당
                fd_context *ctx = malloc(sizeof(fd_context));
                if (!ctx) {
                    close(client_fd);
                    free(conn);
                    continue;
                }
                ctx->conn = conn;
                ctx->type = FD_CLIENT;
                conn->client_ctx = ctx;
                struct epoll_event ev;
                ev.events = EPOLLIN;
                ev.data.ptr = ctx;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
                    perror("epoll_ctl client_fd");
                    close(client_fd);
                    free(ctx);
                    free(conn);
                    continue;
                }
            } else {  // 연결(클라이언트 또는 원격) 이벤트 처리
                fd_context *ctx = (fd_context *)events[i].data.ptr;
                if (!ctx || !ctx->conn)
                    continue;
                struct connection *conn = ctx->conn;
                if (ctx->type == FD_CLIENT) {
                    if (!conn->initialized) {
                        // 초기 요청 처리
                        if (process_initial_request(conn) < 0) {
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->client_fd, NULL);
                            close_connection(ctx);
                            continue;
                        }
                        conn->initialized = 1;
                        // 원격 소켓 이벤트 등록
                        fd_context *rctx = malloc(sizeof(fd_context));
                        if (!rctx) {
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->client_fd, NULL);
                            close_connection(ctx);
                            continue;
                        }
                        rctx->conn = conn;
                        rctx->type = FD_REMOTE;
                        conn->remote_ctx = rctx;
                        struct epoll_event rev;
                        rev.events = EPOLLIN;
                        rev.data.ptr = rctx;
                        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn->remote_fd, &rev) < 0) {
                            perror("epoll_ctl remote_fd");
                            free(rctx);
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->client_fd, NULL);
                            close_connection(ctx);
                            continue;
                        }
                    } else {
                        // 클라이언트 → 원격 서버 데이터 중계
                        char buffer[BUFFER_SIZE];
                        int count = read(conn->client_fd, buffer, BUFFER_SIZE);
                        if (count <= 0) {
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->client_fd, NULL);
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->remote_fd, NULL);
                            close_connection(ctx);
                            continue;
                        }
                        if (write(conn->remote_fd, buffer, count) < 0) {
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->client_fd, NULL);
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->remote_fd, NULL);
                            close_connection(ctx);
                            continue;
                        }
                    }
                } else if (ctx->type == FD_REMOTE) {
                    // 원격 서버 → 클라이언트 데이터 중계
                    char buffer[BUFFER_SIZE];
                    int count = read(conn->remote_fd, buffer, BUFFER_SIZE);
                    if (count <= 0) {
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->client_fd, NULL);
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->remote_fd, NULL);
                        close_connection(ctx);
                        continue;
                    }
                    if (write(conn->client_fd, buffer, count) < 0) {
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->client_fd, NULL);
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->remote_fd, NULL);
                        close_connection(ctx);
                        continue;
                    }
                }
            }
        }
    }
    close(epoll_fd);
}

