// https_client.cpp
// 最小 HTTPS POST 客户端实现：TCP + OpenSSL 1.1 + chunked 解分块 + 三档超时
#include "https_client.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace {

int64_t now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 等待 fd 可读/可写，超时返回 -1（超时）、0（就绪）
int waitFd(int fd, int events, int timeout_ms)
{
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = (short)events;
    pfd.revents = 0;
    return poll(&pfd, 1, timeout_ms);
}

// 非阻塞连接 + 超时；成功返回 0
int tcpConnect(const char *host, const char *port, int timeout_s)
{
    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;               // 板端仅需 IPv4
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res)
        return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    // 非阻塞 connect + poll 超时
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (rc != 0) {
        int pr = waitFd(fd, POLLOUT, timeout_s * 1000);
        if (pr <= 0) {
            close(fd);
            return -1;
        }
        int so_err = 0;
        socklen_t slen = sizeof(so_err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &slen);
        if (so_err != 0) {
            close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, flags);               // 恢复阻塞模式
    return fd;
}

// 从 SSL 读一次，带空闲超时；返回 >0 读到的字节数、0 对端关闭、-1 超时/错误
int sslReadTimeout(SSL *ssl, void *buf, int len, int idle_timeout_ms)
{
    int fd = SSL_get_fd(ssl);
    int pr = waitFd(fd, POLLIN, idle_timeout_ms);
    if (pr <= 0)
        return pr == 0 ? 0 : -1;
    int n = SSL_read(ssl, buf, len);
    return n;   // <=0 交给调用方判断（SSL_ERROR_ZERO_RETURN=对端关闭）
}

// 读 HTTP 响应头（到 \r\n\r\n），返回后 buf 中可能已含部分 body（存到 tail）
bool readHeaders(SSL *ssl, std::string &head, std::string &tail,
                 int idle_ms, int64_t deadline_ms)
{
    char tmp[4096];
    while (true) {
        size_t pos = head.find("\r\n\r\n");
        if (pos != std::string::npos) {
            tail = head.substr(pos + 4);
            head.resize(pos);   // 不含结尾 \r\n\r\n
            return true;
        }
        if (now_ms() > deadline_ms)
            return false;
        int n = sslReadTimeout(ssl, tmp, sizeof(tmp), idle_ms);
        if (n <= 0)
            return false;
        head.append(tmp, (size_t)n);
        if (head.size() > 64 * 1024)   // 响应头防御上限
            return false;
    }
}

// 解析响应头：状态码 / Transfer-Encoding / Content-Length
void parseHead(const std::string &head, int &status, bool &chunked, long &content_len)
{
    chunked = false;
    content_len = -1;
    size_t line_start = 0;
    while (line_start < head.size()) {
        size_t line_end = head.find("\r\n", line_start);
        if (line_end == std::string::npos) line_end = head.size();
        std::string line = head.substr(line_start, line_end - line_start);
        line_start = line_end + 2;

        if (status == 0 && line.compare(0, 5, "HTTP/") == 0) {
            // "HTTP/1.1 200 OK"
            size_t sp = line.find(' ');
            if (sp != std::string::npos)
                status = atoi(line.c_str() + sp + 1);
        } else {
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                std::string val = line.substr(colon + 1);
                while (!val.empty() && (val[0] == ' ' || val[0] == '\t'))
                    val.erase(val.begin());
                if (key == "Transfer-Encoding" && val.find("chunked") != std::string::npos)
                    chunked = true;
                else if (key == "Content-Length")
                    content_len = atol(val.c_str());
            }
        }
    }
}

// 在 body 基础上追加读 n 字节（n 是增量，不是目标大小）
bool readExact(SSL *ssl, std::vector<uint8_t> &body, size_t n,
               int idle_ms, int64_t deadline_ms)
{
    char tmp[4096];
    size_t start = body.size();
    while (body.size() - start < n) {
        if (now_ms() > deadline_ms)
            return false;
        size_t want = n - (body.size() - start);
        if (want > sizeof(tmp)) want = sizeof(tmp);
        int r = sslReadTimeout(ssl, tmp, (int)want, idle_ms);
        if (r <= 0)
            return false;
        body.insert(body.end(), (uint8_t *)tmp, (uint8_t *)tmp + r);
    }
    return true;
}

// 读取一行（\r\n 结尾，返回不含 \r\n 的内容）
bool readLine(SSL *ssl, std::string &pending, std::string &line,
              int idle_ms, int64_t deadline_ms)
{
    char tmp[1024];
    while (true) {
        size_t pos = pending.find("\r\n");
        if (pos != std::string::npos) {
            line = pending.substr(0, pos);
            pending.erase(0, pos + 2);
            return true;
        }
        if (now_ms() > deadline_ms)
            return false;
        int n = sslReadTimeout(ssl, tmp, sizeof(tmp), idle_ms);
        if (n <= 0)
            return false;
        pending.append(tmp, (size_t)n);
        if (pending.size() > 1 * 1024 * 1024)   // 防御
            return false;
    }
}

// chunked 解分块：<hex>\r\n<data>\r\n ... 0\r\n\r\n
bool readChunkedBody(SSL *ssl, std::string &pending, std::vector<uint8_t> &body,
                     int idle_ms, int64_t deadline_ms)
{
    while (true) {
        std::string line;
        if (!readLine(ssl, pending, line, idle_ms, deadline_ms))
            return false;
        // 块大小行是纯 hex（可能带扩展，忽略）
        long chunk_size = strtol(line.c_str(), nullptr, 16);
        if (chunk_size == 0) {
            // 尾随 trailer 直到空行；简单起见读掉一行（通常就是 \r\n）
            std::string endline;
            return readLine(ssl, pending, endline, idle_ms, deadline_ms);
        }
        if (chunk_size < 0 || chunk_size > 16 * 1024 * 1024)
            return false;
        if (!readExact(ssl, body, (size_t)chunk_size, idle_ms, deadline_ms))
            return false;
        // 块数据后的 \r\n
        std::string crlf;
        if (!readLine(ssl, pending, crlf, idle_ms, deadline_ms))
            return false;
    }
}

} // namespace

HttpResponse httpsPost(const std::string &host,
                       const std::string &port,
                       const std::string &path,
                       const std::vector<std::pair<std::string, std::string>> &headers,
                       const std::string &body,
                       int conn_timeout_s,
                       int stream_idle_s,
                       int total_s)
{
    HttpResponse resp;
    int64_t t_start = now_ms();
    int64_t deadline = t_start + (int64_t)total_s * 1000;
    int idle_ms = stream_idle_s * 1000;

    // ---- TCP ----
    int fd = tcpConnect(host.c_str(), port.c_str(), conn_timeout_s);
    if (fd < 0) {
        resp.error = "TCP 连接失败: " + host + ":" + port;
        return resp;
    }

    // ---- TLS ----
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        close(fd);
        resp.error = "SSL_CTX 创建失败";
        return resp;
    }
    // 个人项目：跳过证书校验（板端无 CA 包；商用必须校验证书）
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host.c_str());   // SNI

    // 非阻塞 TLS 握手 + 连接超时
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = SSL_connect(ssl);
    if (rc != 1) {
        int pr = waitFd(fd, POLLOUT, conn_timeout_s * 1000);
        if (pr > 0) {
            // 继续握手直到完成（最多等 conn_timeout）
            int64_t hs_deadline = now_ms() + (int64_t)conn_timeout_s * 1000;
            do {
                rc = SSL_connect(ssl);
                if (rc == 1) break;
                if (SSL_get_error(ssl, rc) != SSL_ERROR_WANT_READ &&
                    SSL_get_error(ssl, rc) != SSL_ERROR_WANT_WRITE)
                    break;
                int ev = (SSL_get_error(ssl, rc) == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
                if (waitFd(fd, ev, 1000) <= 0) break;
            } while (now_ms() < hs_deadline);
        }
    }
    fcntl(fd, F_SETFL, flags);   // 恢复阻塞

    if (rc != 1) {
        resp.error = "TLS 握手失败";
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return resp;
    }

    // ---- 发送请求 ----
    std::string req = "POST " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + "\r\n";
    for (const auto &h : headers)
        req += h.first + ": " + h.second + "\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n";
    req += body;
    if (SSL_write(ssl, req.data(), (int)req.size()) <= 0) {
        resp.error = "请求发送失败";
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return resp;
    }

    // ---- 读响应头 ----
    std::string head, tail;
    if (!readHeaders(ssl, head, tail, idle_ms, deadline)) {
        resp.error = "读响应头超时/失败";
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return resp;
    }
    bool chunked = false;
    long content_len = -1;
    parseHead(head, resp.status, chunked, content_len);

    // ---- 读响应体 ----
    std::string pending = tail;   // 头解析时多读的部分
    bool ok = true;
    if (chunked) {
        ok = readChunkedBody(ssl, pending, resp.body, idle_ms, deadline);
    } else if (content_len >= 0) {
        // 头解析时多读的 tail 先入 body，再补读到 Content-Length
        resp.body.insert(resp.body.end(), (uint8_t *)pending.data(),
                         (uint8_t *)pending.data() + pending.size());
        if ((long)resp.body.size() < content_len)
            ok = readExact(ssl, resp.body, (size_t)(content_len - (long)resp.body.size()),
                           idle_ms, deadline);
        else if ((long)resp.body.size() > content_len)
            resp.body.resize((size_t)content_len);
    } else {
        // 无长度信息：读到对端关闭
        char tmp[4096];
        while (true) {
            if (now_ms() > deadline) { ok = false; break; }
            int n = sslReadTimeout(ssl, tmp, sizeof(tmp), idle_ms);
            if (n <= 0) break;
            resp.body.insert(resp.body.end(), (uint8_t *)tmp, (uint8_t *)tmp + n);
        }
    }
    if (!ok && resp.error.empty())
        resp.error = "读响应体超时/失败";

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);
    return resp;
}
