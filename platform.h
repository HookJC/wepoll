// platform.h —— 统一 epoll 接口，屏蔽平台差异
#ifndef PLATFORM_H
#define PLATFORM_H

#ifdef _WIN32
    // Windows：使用 wepoll
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include "wepoll.h"          // wepoll 头文件
    typedef HANDLE epoll_handle_t;
    typedef SOCKET socket_t;
    #define CLOSE_SOCKET closesocket
    #define EPOLL_CLOSE epoll_close  // wepoll 用 epoll_close 关闭端口

    // wepoll 的 epoll_ctl 第 3 个参数是 SOCKET，不是 fd
    #define EPOLL_CTL_ADD_SOCK(ep, s, ev) \
        epoll_ctl((ep), EPOLL_CTL_ADD, (SOCKET)(s), (ev))

#else
    // Linux / macOS
    #include <sys/epoll.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    typedef int epoll_handle_t;
    typedef int socket_t;
    #define CLOSE_SOCKET close
    #define EPOLL_CLOSE close
    #define EPOLL_CTL_ADD_SOCK(ep, s, ev) \
        epoll_ctl((ep), EPOLL_CTL_ADD, (int)(s), (ev))

    // macOS 用 kqueue，这里简化处理
    #ifdef __APPLE__
        // 实际项目可替换为 kqueue 封装，此处略
    #endif
#endif

// 通用事件结构（wepoll 的 epoll_event 与 Linux 一致）
#ifdef _WIN32
    // wepoll 的 epoll_event 定义在 wepoll.h 中，直接使用
#else
    // Linux 的 epoll_event 已包含在 sys/epoll.h 中
#endif

#endif // PLATFORM_H
