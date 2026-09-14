// https_client.h
// 最小 HTTPS POST 客户端（板端无 curl/无 CA 证书，自研 TCP + OpenSSL 1.1）
//
// 设计要点（个人项目权衡，详见技术文档 17 章）：
//   - 跳过服务器证书校验（SSL_VERIFY_NONE）：板端无 CA 证书包，个人项目
//     数据无价值，接受中间人风险；商用必须带 CA bundle
//   - 每次请求新建连接（Connection: close），不做 keep-alive 复用：
//     多轮对话间隔远超服务器 keep-alive 空闲时限，复用无意义
//   - 不发送 Accept-Encoding 头 → 服务端不压缩，省去 gzip 解压
//   - 支持 Transfer-Encoding: chunked 解分块（DeepSeek/火山响应均为 chunked）
//   - 三档超时：连接 / 流空闲（每次 SSL_read 的等待）/ 请求总时长
#ifndef HTTPS_CLIENT_H
#define HTTPS_CLIENT_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct HttpResponse {
    int status = 0;                        // HTTP 状态码；0 = 网络/超时等传输层错误
    std::string error;                     // 错误描述（status==0 时有效）
    std::vector<uint8_t> body;             // 响应体（已解 chunked）
};

// 同步阻塞 HTTPS POST。
// host 不含端口（如 "api.deepseek.com"），port 传 "443"。
// 返回后由调用方检查 status / error。
HttpResponse httpsPost(const std::string &host,
                       const std::string &port,
                       const std::string &path,
                       const std::vector<std::pair<std::string, std::string>> &headers,
                       const std::string &body,
                       int conn_timeout_s = 10,
                       int stream_idle_s = 15,
                       int total_s = 60);

#endif // HTTPS_CLIENT_H
