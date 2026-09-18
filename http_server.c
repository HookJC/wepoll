/* http_server.c
 * 简易跨平台 HTTP 服务器，基于 wepoll（Windows）/ epoll（Linux）
 * 支持目录列表和文件下载（含断点续传）
 *
 * 编译：
 *   Windows (MSVC):  cl http_server.c wepoll.c /link ws2_32.lib
 *   Windows (MinGW): gcc http_server.c wepoll.c -o http_server.exe -lws2_32
 *   Linux:           gcc http_server.c -o http_server
 *
 * 用法：
 *   http_server [-p port] [-r root_dir]
 *   默认端口 1990，默认根目录 ./www
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

/* ============================================================
 *  平台适配层
 * ============================================================ */
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include "wepoll.h"

    typedef SOCKET        socket_t;
    typedef HANDLE        epoll_t;
    typedef int           socklen_t_win; /* 占位 */

    #define CLOSE_SOCKET(s)     closesocket(s)
    #define EPOLL_CLOSE(e)      epoll_close(e)
    #define IS_WOULDBLOCK()     (WSAGetLastError() == WSAEWOULDBLOCK)
    #define SOCK_ERR            SOCKET_ERROR
    #define INVALID_SOCK_VAL    INVALID_SOCKET

    static void sock_init(void) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    static void sock_cleanup(void) { WSACleanup(); }

    static void set_nonblocking(socket_t s) {
        u_long mode = 1;
        ioctlsocket(s, FIONBIO, &mode);
    }
#else
    #include <sys/epoll.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>

    typedef int  socket_t;
    typedef int  epoll_t;

    #define CLOSE_SOCKET(s)     close(s)
    #define EPOLL_CLOSE(e)      close(e)
    #define IS_WOULDBLOCK()     (errno == EAGAIN || errno == EWOULDBLOCK)
    #define SOCK_ERR            (-1)
    #define INVALID_SOCK_VAL    (-1)

    static void sock_init(void)    { /* no-op */ }
    static void sock_cleanup(void) { /* no-op */ }

    static void set_nonblocking(socket_t s) {
        int flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
    }
#endif

/* ============================================================
 *  常量与全局配置
 * ============================================================ */
#define DEFAULT_PORT    1990
#define DEFAULT_ROOT    "./www"
#define MAX_EVENTS      1024
#define BUF_SIZE        65536
#define REQ_BUF_SIZE    8192
#define HDR_BUF_SIZE    2048

static const char *g_root_dir = DEFAULT_ROOT;

/* ============================================================
 *  连接状态机
 * ============================================================ */
typedef enum {
    ST_READ_HEADER,   /* 读 HTTP 请求头 */
    ST_WRITE,         /* 写响应（头 + 体） */
} conn_state_t;

typedef struct conn {
    socket_t     sock;
    conn_state_t state;

    /* 请求缓冲 */
    char   req_buf[REQ_BUF_SIZE];
    size_t req_len;

    /* 响应头 */
    char   hdr_buf[HDR_BUF_SIZE];
    size_t hdr_len;
    size_t hdr_sent;

    /* 响应体 —— 文件模式 */
    FILE      *fp;
    long long  body_total;       /* 需要发送的总字节数 */
    long long  body_sent_total;  /* 已发送的字节数 */
    char       body_buf[BUF_SIZE];
    size_t     body_chunk_len;   /* 当前块大小 */
    size_t     body_chunk_sent;  /* 当前块已发送 */

    /* 响应体 —— 内存模式（目录列表 HTML 等） */
    char  *body_mem;
    size_t body_mem_len;

    struct conn *next;
} conn_t;

static conn_t *g_conns = NULL;

static void conn_list_add(conn_t *c) {
    c->next = g_conns;
    g_conns = c;
}
static void conn_list_remove(conn_t *c) {
    conn_t **pp = &g_conns;
    while (*pp && *pp != c) pp = &(*pp)->next;
    if (*pp) *pp = c->next;
}

/* ============================================================
 *  MIME 类型
 * ============================================================ */
static const char *get_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    if (!strcasecmp(ext, ".html") || !strcasecmp(ext, ".htm")) return "text/html; charset=utf-8";
    if (!strcasecmp(ext, ".css"))  return "text/css";
    if (!strcasecmp(ext, ".js"))   return "application/javascript";
    if (!strcasecmp(ext, ".json")) return "application/json";
    if (!strcasecmp(ext, ".png"))  return "image/png";
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(ext, ".gif"))  return "image/gif";
    if (!strcasecmp(ext, ".svg"))  return "image/svg+xml";
    if (!strcasecmp(ext, ".ico"))  return "image/x-icon";
    if (!strcasecmp(ext, ".txt"))  return "text/plain; charset=utf-8";
    if (!strcasecmp(ext, ".pdf"))  return "application/pdf";
    if (!strcasecmp(ext, ".zip"))  return "application/zip";
    if (!strcasecmp(ext, ".gz"))   return "application/gzip";
    if (!strcasecmp(ext, ".mp4"))  return "video/mp4";
    if (!strcasecmp(ext, ".mp3"))  return "audio/mpeg";
    return "application/octet-stream";
}

/* ============================================================
 *  工具函数
 * ============================================================ */
static void format_size(long long bytes, char *out, size_t out_len) {
    if (bytes < 1024)                snprintf(out, out_len, "%lld B", bytes);
    else if (bytes < 1048576)        snprintf(out, out_len, "%.1f KB", bytes / 1024.0);
    else if (bytes < 1073741824LL)   snprintf(out, out_len, "%.1f MB", bytes / 1048576.0);
    else                             snprintf(out, out_len, "%.1f GB", bytes / 1073741824.0);
}

/* URL 解码：把 %xx 还原成字节，'+' 还原成空格 */
static void url_decode(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            int hi = r[1], lo = r[2];
            #define HEX(c) ((c >= '0' && c <= '9') ? c - '0' : \
                            (c >= 'a' && c <= 'f') ? c - 'a' + 10 : \
                            (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0)
            *w++ = (char)((HEX(hi) << 4) | HEX(lo));
            #undef HEX
            r += 3;
        } else if (*r == '+') {
            *w++ = ' ';
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* HTML 转义 */
static void html_escape(const char *in, char *out, size_t out_len) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 8 < out_len; i++) {
        switch (in[i]) {
            case '<':  memcpy(out + j, "&lt;",   4); j += 4; break;
            case '>':  memcpy(out + j, "&gt;",   4); j += 4; break;
            case '&':  memcpy(out + j, "&amp;",  5); j += 5; break;
            case '"':  memcpy(out + j, "&quot;", 6); j += 6; break;
            default:   out[j++] = in[i]; break;
        }
    }
    out[j] = '\0';
}

/* ============================================================
 *  epoll 事件管理
 * ============================================================ */
static void conn_close(epoll_t epfd, conn_t *c) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->sock, NULL);
    CLOSE_SOCKET(c->sock);
    if (c->fp)       fclose(c->fp);
    if (c->body_mem) free(c->body_mem);
    conn_list_remove(c);
    free(c);
}

static int conn_set_events(epoll_t epfd, conn_t *c, uint32_t events) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events   = events;
    ev.data.ptr = c;
    return epoll_ctl(epfd, EPOLL_CTL_MOD, c->sock, &ev);
}

/* ============================================================
 *  目录列表 HTML 生成
 * ============================================================ */
static char *generate_directory_listing(const char *dir_path, const char *url_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) return NULL;

    size_t cap = 8192;
    char *html = malloc(cap);
    if (!html) { closedir(dir); return NULL; }
    size_t len = 0;

    char url_esc[2048];
    html_escape(url_path, url_esc, sizeof(url_esc));

    #define APPEND(...) do {                                      \
        int _n = snprintf(html + len, cap - len, __VA_ARGS__);    \
        if (_n > 0) {                                             \
            if ((size_t)_n >= cap - len) {                        \
                cap = (len + _n + 1) * 2;                         \
                html = realloc(html, cap);                        \
                if (!html) { closedir(dir); return NULL; }        \
                _n = snprintf(html + len, cap - len, __VA_ARGS__);\
            }                                                     \
            len += _n;                                            \
        }                                                         \
    } while (0)

    APPEND("<!DOCTYPE html>\n"
           "<html><head><meta charset=\"utf-8\">\n"
           "<title>Index of %s</title>\n"
           "<style>\n"
           "body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;"
           "max-width:900px;margin:40px auto;padding:0 20px;color:#333}\n"
           "h1{border-bottom:2px solid #0066cc;padding-bottom:8px;color:#0066cc}\n"
           "table{border-collapse:collapse;width:100%%}\n"
           "th,td{text-align:left;padding:8px 12px;border-bottom:1px solid #eee}\n"
           "th{background:#0066cc;color:#fff}\n"
           "tr:hover{background:#f0f8ff}\n"
           "a{color:#0066cc;text-decoration:none}\n"
           "a:hover{text-decoration:underline}\n"
           "td.size{text-align:right;font-family:monospace;color:#666}\n"
           "</style></head><body>\n"
           "<h1>Index of %s</h1>\n"
           "<table><tr><th>Name</th><th class=\"size\">Size</th>"
           "<th>Type</th></tr>\n",
           url_esc, url_esc);

    if (strcmp(url_path, "/") != 0) {
        APPEND("<tr><td><a href=\"%s../\">../</a></td>"
               "<td class=\"size\">-</td><td>dir</td></tr>\n", url_esc);
    }

    /* 先收集条目，排序（简单按目录优先、名字升序） */
    typedef struct { char name[512]; int is_dir; long long size; } ent_t;
    ent_t *ents = NULL;
    size_t nent = 0, cap_ent = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (nent >= cap_ent) {
            cap_ent = cap_ent ? cap_ent * 2 : 32;
            ents = realloc(ents, cap_ent * sizeof(*ents));
        }
        snprintf(ents[nent].name, sizeof(ents[nent].name), "%s", de->d_name);
        ents[nent].is_dir = S_ISDIR(st.st_mode);
        ents[nent].size   = S_ISREG(st.st_mode) ? (long long)st.st_size : 0;
        nent++;
    }
    closedir(dir);

    for (size_t i = 0; i < nent; i++) {
        for (size_t j = i + 1; j < nent; j++) {
            int swap = 0;
            if (ents[i].is_dir != ents[j].is_dir)
                swap = (ents[j].is_dir && !ents[i].is_dir);
            else if (strcmp(ents[i].name, ents[j].name) > 0)
                swap = 1;
            if (swap) { ent_t t = ents[i]; ents[i] = ents[j]; ents[j] = t; }
        }
    }

    for (size_t i = 0; i < nent; i++) {
        char name_esc[2048];
        html_escape(ents[i].name, name_esc, sizeof(name_esc));

        char size_str[32] = "-";
        if (!ents[i].is_dir)
            format_size(ents[i].size, size_str, sizeof(size_str));

        APPEND("<tr><td><a href=\"%s%s%s\">%s%s</a></td>"
               "<td class=\"size\">%s</td><td>%s</td></tr>\n",
               url_esc, name_esc, ents[i].is_dir ? "/" : "",
               name_esc, ents[i].is_dir ? "/" : "",
               size_str, ents[i].is_dir ? "dir" : "file");
    }
    free(ents);

    APPEND("</table></body></html>\n");

    #undef APPEND
    return html;
}

/* ============================================================
 *  尝试写响应（返回 1 = 完成，0 = 需要继续等待 EPOLLOUT）
 * ============================================================ */
static int try_write(conn_t *c) {
    /* 1. 发送响应头 */
    while (c->hdr_sent < c->hdr_len) {
        int n = send(c->sock,
                     c->hdr_buf + c->hdr_sent,
                     (int)(c->hdr_len - c->hdr_sent), 0);
        if (n < 0) {
            if (IS_WOULDBLOCK()) return 0;
            return 1;  /* 出错，视为完成（调用方关闭连接） */
        }
        if (n == 0) return 1;
        c->hdr_sent += (size_t)n;
    }

    /* 2. 发送响应体 —— 文件模式 */
    if (c->fp) {
        while (c->body_sent_total < c->body_total) {
            if (c->body_chunk_sent >= c->body_chunk_len) {
                long long remaining = c->body_total - c->body_sent_total;
                size_t to_read = (remaining < BUF_SIZE) ? (size_t)remaining : BUF_SIZE;
                size_t nr = fread(c->body_buf, 1, to_read, c->fp);
                if (nr == 0) return 1;  /* 意外 EOF */
                c->body_chunk_len  = nr;
                c->body_chunk_sent = 0;
            }

            int n = send(c->sock,
                         c->body_buf + c->body_chunk_sent,
                         (int)(c->body_chunk_len - c->body_chunk_sent), 0);
            if (n < 0) {
                if (IS_WOULDBLOCK()) return 0;
                return 1;
            }
            if (n == 0) return 1;
            c->body_chunk_sent  += (size_t)n;
            c->body_sent_total  += n;
        }
        return 1;
    }

    /* 3. 发送响应体 —— 内存模式 */
    if (c->body_mem) {
        while (c->body_sent_total < (long long)c->body_mem_len) {
            int n = send(c->sock,
                         c->body_mem + c->body_sent_total,
                         (int)(c->body_mem_len - (size_t)c->body_sent_total), 0);
            if (n < 0) {
                if (IS_WOULDBLOCK()) return 0;
                return 1;
            }
            if (n == 0) return 1;
            c->body_sent_total += n;
        }
        return 1;
    }

    return 1;  /* 只有响应头，已发完 */
}

/* 尝试写并管理事件；返回 1 = 完成（调用方关闭），0 = 继续等待 */
static int send_response(epoll_t epfd, conn_t *c) {
    if (try_write(c)) return 1;
    if (conn_set_events(epfd, c, EPOLLOUT) < 0) return 1;
    return 0;
}

/* ============================================================
 *  响应构造
 * ============================================================ */

/* 内联响应（头 + 小 body 拼在一起） */
static int send_simple_error(epoll_t epfd, conn_t *c, int code, const char *msg) {
    const char *reason = "Error";
    switch (code) {
        case 400: reason = "Bad Request";           break;
        case 403: reason = "Forbidden";             break;
        case 404: reason = "Not Found";             break;
        case 405: reason = "Method Not Allowed";    break;
        case 500: reason = "Internal Server Error"; break;
    }

    char body[512];
    int blen = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>%d %s</title></head><body>"
        "<h1>%d %s</h1><p>%s</p></body></html>\n",
        code, reason, code, reason, msg);

    c->hdr_len = snprintf(c->hdr_buf, sizeof(c->hdr_buf),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n"
        "%s",
        code, reason, blen, body);
    c->hdr_sent = 0;
    c->state    = ST_WRITE;
    return send_response(epfd, c);
}

/* 文件响应，支持 Range */
static int send_file_response(epoll_t epfd, conn_t *c,
                              const char *path, const char *mime,
                              long long range_start, long long range_end) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return send_simple_error(epfd, c, 500, "Failed to open file");

    fseek(fp, 0, SEEK_END);
    long long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    int is_range = (range_start >= 0);
    if (!is_range) {
        range_start = 0;
        range_end   = file_size - 1;
    }
    if (range_end < 0 || range_end >= file_size) range_end = file_size - 1;
    if (range_start > range_end) {
        fclose(fp);
        return send_simple_error(epfd, c, 400, "Invalid Range");
    }
    long long content_len = range_end - range_start + 1;

    /* 取文件名用于 Content-Disposition */
    const char *name = strrchr(path, '/');
#ifdef _WIN32
    if (!name) { const char *b = strrchr(path, '\\'); if (b) name = b; }
#endif
    name = name ? name + 1 : path;

    if (is_range) {
        c->hdr_len = snprintf(c->hdr_buf, sizeof(c->hdr_buf),
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lld\r\n"
            "Content-Range: bytes %lld-%lld/%lld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Content-Disposition: attachment; filename=\"%s\"\r\n"
            "Connection: close\r\n\r\n",
            mime, content_len, range_start, range_end, file_size, name);
    } else {
        c->hdr_len = snprintf(c->hdr_buf, sizeof(c->hdr_buf),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Content-Disposition: attachment; filename=\"%s\"\r\n"
            "Connection: close\r\n\r\n",
            mime, content_len, name);
    }

    c->hdr_sent         = 0;
    c->fp               = fp;
    c->body_total       = content_len;
    c->body_sent_total  = 0;
    c->body_chunk_len   = 0;
    c->body_chunk_sent  = 0;
    c->state            = ST_WRITE;

    fseek(fp, (long)range_start, SEEK_SET);
    return send_response(epfd, c);
}

/* 目录响应 */
static int send_dir_response(epoll_t epfd, conn_t *c,
                             const char *fs_path, const char *url_path) {
    char *html = generate_directory_listing(fs_path, url_path);
    if (!html) return send_simple_error(epfd, c, 500, "Cannot list directory");

    size_t html_len = strlen(html);
    c->body_mem     = html;
    c->body_mem_len = html_len;

    c->hdr_len = snprintf(c->hdr_buf, sizeof(c->hdr_buf),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        html_len);

    c->hdr_sent        = 0;
    c->body_sent_total = 0;
    c->state           = ST_WRITE;
    return send_response(epfd, c);
}

/* ============================================================
 *  请求解析 → 生成响应
 * ============================================================ */
static int prepare_response(epoll_t epfd, conn_t *c) {
    char method[16], url_path[2048], version[16];
    if (sscanf(c->req_buf, "%15s %2047s %15s",
               method, url_path, version) != 3) {
        return send_simple_error(epfd, c, 400, "Malformed request");
    }

    if (strcmp(method, "GET") != 0) {
        return send_simple_error(epfd, c, 405, "Only GET is supported");
    }

    /* 解析 Range 头 */
    long long rs = -1, re = -1;
    char *rng = strstr(c->req_buf, "Range: bytes=");
    if (!rng) rng = strstr(c->req_buf, "range: bytes=");
    if (rng) {
        rng += 13;
        /* 只处理 "start-end" / "start-" 两种形式 */
        if (sscanf(rng, "%lld-%lld", &rs, &re) < 1) rs = -1;
        if (re <= 0) re = -1;
    }

    /* 去掉查询串 */
    char *q = strchr(url_path, '?');
    if (q) *q = '\0';

    /* URL 解码 */
    url_decode(url_path);

    /* 安全检查：禁止 ".." 路径穿越 */
    if (strstr(url_path, "..")) {
        return send_simple_error(epfd, c, 403, "Forbidden");
    }

    /* 拼接文件系统路径 */
    char fs_path[4096];
    if (strcmp(url_path, "/") == 0)
        snprintf(fs_path, sizeof(fs_path), "%s", g_root_dir);
    else
        snprintf(fs_path, sizeof(fs_path), "%s%s", g_root_dir, url_path);

    struct stat st;
    if (stat(fs_path, &st) != 0) {
        return send_simple_error(epfd, c, 404, "Not Found");
    }

    if (S_ISDIR(st.st_mode)) {
        /* 目录：若存在 index.html 则返回它 */
        char index_path[4096];
        snprintf(index_path, sizeof(index_path), "%s/index.html", fs_path);
        struct stat st2;
        if (stat(index_path, &st2) == 0 && S_ISREG(st2.st_mode)) {
            return send_file_response(epfd, c, index_path,
                                      "text/html; charset=utf-8", -1, -1);
        }
        return send_dir_response(epfd, c, fs_path, url_path);
    }

    if (S_ISREG(st.st_mode)) {
        return send_file_response(epfd, c, fs_path,
                                  get_mime_type(fs_path), rs, re);
    }

    return send_simple_error(epfd, c, 403, "Forbidden");
}

/* ============================================================
 *  读请求头
 * ============================================================ */
static void handle_read_header(epoll_t epfd, conn_t *c) {
    if (c->req_len >= sizeof(c->req_buf) - 1) {
        conn_close(epfd, c);
        return;
    }

    int n = recv(c->sock,
                 c->req_buf + c->req_len,
                 (int)(sizeof(c->req_buf) - 1 - c->req_len), 0);

    if (n == 0) {
        /* 对端关闭 */
        conn_close(epfd, c);
        return;
    }
    if (n < 0) {
        if (IS_WOULDBLOCK()) return;
        conn_close(epfd, c);
        return;
    }

    c->req_len += (size_t)n;
    c->req_buf[c->req_len] = '\0';

    if (strstr(c->req_buf, "\r\n\r\n")) {
        /* 请求头完整，构造响应 */
        if (prepare_response(epfd, c) != 0) {
            conn_close(epfd, c);
        }
        /* prepare_response 返回 0 时已注册 EPOLLOUT，连接继续 */
        return;
    }
    /* 请求头未完整，等下次 EPOLLIN */
}

/* ============================================================
 *  主函数
 * ============================================================ */
static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -p, --port <port>   TCP listening port (1-65535), default: %d\n"
        "  -r, --root <dir>    Root directory to serve, default: %s\n"
        "  -h, --help          Show this help message\n"
        "\n"
        "Examples:\n"
        "  %s\n"
        "  %s -p 8080\n"
        "  %s -r /var/www\n"
        "  %s -p 8080 -r /var/www\n",
        prog, DEFAULT_PORT, DEFAULT_ROOT, prog, prog, prog, prog);
}

static int check_root_dir(const char *dir) {
    struct stat st;
    if (stat(dir, &st) != 0) {
        fprintf(stderr, "Error: root directory '%s' does not exist\n", dir);
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: '%s' is not a directory\n", dir);
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    const char *root = DEFAULT_ROOT;

    /* ---- 解析命令行 ---- */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--port")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -p/--port requires an argument\n");
                print_usage(argv[0]);
                return 1;
            }
            char *end = NULL;
            long p = strtol(argv[++i], &end, 10);
            if (!end || *end != '\0' || p < 1 || p > 65535) {
                fprintf(stderr, "Error: invalid port '%s'\n", argv[i]);
                return 1;
            }
            port = (int)p;
        } else if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--root")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -r/--root requires an argument\n");
                print_usage(argv[0]);
                return 1;
            }
            root = argv[++i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (check_root_dir(root) != 0) return 1;
    g_root_dir = root;

    sock_init();

    /* ---- 创建监听 socket ---- */
    socket_t listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == INVALID_SOCK_VAL) {
        perror("socket");
        sock_cleanup();
        return 1;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((unsigned short)port);

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        CLOSE_SOCKET(listen_sock);
        sock_cleanup();
        return 1;
    }
    if (listen(listen_sock, SOMAXCONN) < 0) {
        perror("listen");
        CLOSE_SOCKET(listen_sock);
        sock_cleanup();
        return 1;
    }
    set_nonblocking(listen_sock);

    /* ---- 创建 epoll ---- */
    epoll_t epfd = epoll_create(10);
#ifdef _WIN32
    if (epfd == NULL) {
#else
    if (epfd < 0) {
#endif
        perror("epoll_create");
        CLOSE_SOCKET(listen_sock);
        sock_cleanup();
        return 1;
    }

    /* 监听 socket 用 data.ptr == NULL 标记 */
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events   = EPOLLIN;
    ev.data.ptr = NULL;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_sock, &ev) < 0) {
        perror("epoll_ctl ADD listen");
        EPOLL_CLOSE(epfd);
        CLOSE_SOCKET(listen_sock);
        sock_cleanup();
        return 1;
    }

    printf("HTTP server listening on port %d\n", port);
    printf("Serving directory: %s\n", g_root_dir);
    printf("Open http://localhost:%d/ in your browser\n", port);
    fflush(stdout);

    /* ---- 事件循环 ---- */
    struct epoll_event events[MAX_EVENTS];
    while (1) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            void    *ptr = events[i].data.ptr;
            uint32_t evs = events[i].events;

            /* ---- 监听 socket：接受新连接 ---- */
            if (ptr == NULL) {
                while (1) {
                    struct sockaddr_in cli;
#ifdef _WIN32
                    int clen = sizeof(cli);
#else
                    socklen_t clen = sizeof(cli);
#endif
                    socket_t cs = accept(listen_sock,
                                         (struct sockaddr *)&cli, &clen);
                    if (cs == INVALID_SOCK_VAL) break;

                    set_nonblocking(cs);

                    conn_t *nc = calloc(1, sizeof(*nc));
                    if (!nc) { CLOSE_SOCKET(cs); continue; }
                    nc->sock  = cs;
                    nc->state = ST_READ_HEADER;
                    conn_list_add(nc);

                    struct epoll_event ev2;
                    memset(&ev2, 0, sizeof(ev2));
                    ev2.events   = EPOLLIN | EPOLLRDHUP;
                    ev2.data.ptr = nc;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, cs, &ev2) < 0) {
                        conn_close(epfd, nc);
                    }
                }
                continue;
            }

            /* ---- 普通连接 ---- */
            conn_t *c = (conn_t *)ptr;

            if (evs & (EPOLLERR | EPOLLHUP)) {
                conn_close(epfd, c);
                continue;
            }

            if (c->state == ST_READ_HEADER) {
                if (evs & (EPOLLIN | EPOLLRDHUP)) {
                    handle_read_header(epfd, c);
                    /* handle_read_header 可能已关闭 c */
                }
            } else if (c->state == ST_WRITE) {
                if (evs & EPOLLOUT) {
                    if (send_response(epfd, c) != 0) {
                        conn_close(epfd, c);
                    }
                }
            }
        }
    }

    /* 清理 */
    conn_t *c = g_conns;
    while (c) {
        conn_t *next = c->next;
        CLOSE_SOCKET(c->sock);
        if (c->fp)       fclose(c->fp);
        if (c->body_mem) free(c->body_mem);
        free(c);
        c = next;
    }
    EPOLL_CLOSE(epfd);
    CLOSE_SOCKET(listen_sock);
    sock_cleanup();
    return 0;
}
