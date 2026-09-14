# 09. 项目精讲:HTTPS 客户端——手写网络编程与协议解析

> 目标:读懂 https_client.h/.cpp(约 400 行)。这是纯 C 也能写的代码,
> 但 C++ 的组织方式让"裸 socket 编程"变得可控。学完你会理解:
> HTTPS = TCP + TLS + HTTP 三层叠加,以及每一层的超时都要自己做。

---

## 9.1 为什么手写而不是用 curl

板端 buildroot **没有 curl、没有 CA 证书**。选择:交叉编译 libcurl(引入一大坨依赖)或手写最小客户端(400 行)。项目选了后者——**嵌入式开发的核心权衡:依赖换可控**。

分层视角:

```
应用层:  POST /chat/completions + JSON body(我们自己拼)
HTTP层:  请求行/头/响应状态/Transfer-Encoding: chunked
TLS层:   加密握手/SNI/证书(跳过校验)
TCP层:   socket + connect + 超时
```

---

## 9.2 接口设计:一个函数干完一件事

```cpp
// server/cpp/https_client.h
struct HttpResponse {
    int status = 0;                  // HTTP 状态码;0 = 传输层错误
    std::string error;               // 错误描述
    std::vector<uint8_t> body;       // 响应体(已解 chunked)
};

HttpResponse httpsPost(const std::string &host,
                       const std::string &port,
                       const std::string &path,
                       const std::vector<std::pair<std::string, std::string>> &headers,
                       const std::string &body,
                       int conn_timeout_s = 10,
                       int stream_idle_s = 15,
                       int total_s = 60);
```

**接口即契约**:
- 成功/失败统一在返回结构里(status + error),**不抛异常**——嵌入式项目普遍禁用异常(板端 gcc 常配 -fno-exceptions),错误显式返回
- 默认参数:三档超时带默认值,调用方不关心就不用写(项目内部全用默认)
- `std::vector<std::pair<string,string>>` 表示请求头列表——对比 C 的"指针数组+结束哨兵",类型安全且长度自明

---

## 9.3 TCP 连接:非阻塞 connect + 超时(第一个必须掌握的技能)

```cpp
// tcpConnect()
struct addrinfo hints, *res = nullptr;
memset(&hints, 0, sizeof(hints));
hints.ai_family = AF_INET;               // 仅 IPv4(板端场景明确)
hints.ai_socktype = SOCK_STREAM;
getaddrinfo(host, port, &hints, &res);   // DNS + 地址解析

int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
int flags = fcntl(fd, F_GETFL, 0);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);  // ① 置非阻塞
int rc = connect(fd, res->ai_addr, res->ai_addrlen);
if (rc != 0 && errno != EINPROGRESS) { close(fd); return -1; }   // 立即失败

int pr = waitFd(fd, POLLOUT, timeout_s * 1000);   // ② poll 等可写=连接完成
if (pr <= 0) { close(fd); return -1; }            // 超时
int so_err = 0; getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, ...);
if (so_err != 0) { close(fd); return -1; }        // ③ 查真实结果
fcntl(fd, F_SETFL, flags);                        // ④ 恢复阻塞
```

**为什么必须非阻塞 connect**:阻塞 connect 的默认超时是系统级(几分钟),板端断网会挂死整个对话线程。非阻塞 + poll 把超时握在自己手里。**任何"可能卡住的系统调用"都要这么处理**——这是网络编程第一课,面试也常问。

`waitFd` 就是 poll 的薄封装:

```cpp
int waitFd(int fd, int events, int timeout_ms) {
    struct pollfd pfd;
    pfd.fd = fd; pfd.events = (short)events; pfd.revents = 0;
    return poll(&pfd, 1, timeout_ms);
}
```

---

## 9.4 TLS:三行建立加密层 + 一个必须知道的坑

```cpp
SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);   // ← 跳过证书校验!
SSL *ssl = SSL_new(ctx);
SSL_set_fd(ssl, fd);
SSL_set_tlsext_host_name(ssl, host.c_str());         // SNI:一个 IP 多证书的通行证
// 非阻塞握手(同 connect 的模式):
SSL_connect(ssl) → 循环 + poll 超时,直到 rc==1
```

**SNI 是必须的**:现代 CDN 一台服务器挂着几百个网站的证书,不告诉它"我要访问哪个域名"(SNI),握手直接失败——这是裸 socket + TLS 最常踩的坑,项目代码注释里明确写了。

**跳过证书校验是个人项目的妥协**(注释注明:商用必须带 CA bundle):没有校验证书意味着理论上可以被中间人截胡。**知道自己在做什么、把妥协写进注释**——这也是工程素养的一部分。

---

## 9.5 HTTP 响应解析:chunked 解分块(项目的"名场面")

### 响应头 vs 响应体的边界

```cpp
// readHeaders():读到 "\r\n\r\n" 为止
size_t pos = head.find("\r\n\r\n");
if (pos != std::string::npos) {
    tail = head.substr(pos + 4);   // 多读的部分要留着!它已是 body 开头
    head.resize(pos);
    return true;
}
```

**多读的字节不能丢**:SSL_read 按块读,常常一次读进"头+半个 body"。tail 交接给 body 解析器——这是流式解析的经典细节,丢字节的 bug 极难查。

### chunked 的格式与解析

```
HTTP/1.1 200 OK
Transfer-Encoding: chunked
<空行>
562\r\n               ← 块大小(十六进制)
<562 字节数据>\r\n
278\r\n
<278 字节数据>\r\n
0\r\n                 ← 结束块
\r\n
```

```cpp
bool readChunkedBody(SSL *ssl, std::string &pending, std::vector<uint8_t> &body, ...) {
    while (true) {
        std::string line;
        readLine(ssl, pending, line, ...);            // 读块大小行
        long chunk_size = strtol(line.c_str(), nullptr, 16);   // 十六进制转数字
        if (chunk_size == 0) {                        // 结束
            std::string endline;
            return readLine(ssl, pending, endline, ...);   // 吃掉结尾空行
        }
        if (chunk_size < 0 || chunk_size > 16*1024*1024) return false;  // 防御
        readExact(ssl, body, (size_t)chunk_size, ...);  // 按字节数精确读
        std::string crlf;
        readLine(ssl, pending, crlf, ...);              // 吃掉块尾 \r\n
    }
}
```

### 我们真实踩过的坑(第 12 章会再讲,这里先看教训)

```cpp
// 有 bug 的版本:
bool readExact(..., size_t n, ...) {
    while (body.size() < n) { ... }    // ← 语义是"补到 n 字节"
}
// chunked 循环每次传"新增量",body 已超过 n 时循环不执行 → 小块全被跳过
// 结果:响应体只剩最大那个块,SSE 永远等不到 [DONE]

// 修复:记录起始位置,按"增量"读:
size_t start = body.size();
while (body.size() - start < n) { ... }
```

**教训**:函数的语义(增量 vs 目标)必须想清楚再写;集成前先在 PC 上写独立测试程序连真实服务器验证——这个 bug 就是 PC 测试程序抓出来的。

---

## 9.6 三档超时的实现:所有阻塞都有限期

| 档位 | 保护什么 | 实现 |
|---|---|---|
| 连接超时 10s | connect + TLS 握手 | 非阻塞 + poll |
| 流空闲 15s | 每次 SSL_read 的等待 | sslReadTimeout 里 poll |
| 总时长 60s | 整个请求兜底 | deadline 时间戳,每步检查 |

```cpp
int sslReadTimeout(SSL *ssl, void *buf, int len, int idle_timeout_ms) {
    int fd = SSL_get_fd(ssl);
    int pr = waitFd(fd, POLLIN, idle_timeout_ms);   // 先 poll 可读
    if (pr <= 0) return pr == 0 ? 0 : -1;
    return SSL_read(ssl, buf, len);                  // 再读(此时不会阻塞)
}
```

**poll 之后再 SSL_read**:SSL_read 可能因为 TLS 记录边界要读多次,但先 poll 保证"至少有一次不阻塞"。裸 TLS 编程的又一经典模式。

---

## 9.7 思考题

1. 把 `SSL_CTX_set_verify` 改回默认(校验证书),板端会怎样?要让板端能验证,需要给系统补什么?(提示:CA 证书包)
2. 为什么项目里每请求新建连接(Connection: close),不复用?多轮对话场景下 keep-alive 复用省了什么、怕什么?
3. `readLine` 用 find("\r\n") 逐字节累积——如果服务器只发 "\n"(Unix 风格)不发 "\r\n",这段代码会怎样?怎么防御?
4. total 60s 的 deadline 在循环里每步都检查——漏掉哪个检查点,程序会在哪卡死?

---

## 一句话总结

网络编程 = 把"可能卡住"的一切变成"有限等待":非阻塞 connect、poll+SSL_read、三档超时;协议解析 = 字节流的精确记账:头体边界、chunked 分块、多读字节交接。手写一次,curl 的原理你就全懂了。
