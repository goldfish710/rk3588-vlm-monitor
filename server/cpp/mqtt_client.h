#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include "mqtt_thread.h"
bool mqtt_connect();


bool mqtt_is_connected();

// MQTTEvent → JSON payload（旧字段 + 非空才追加的新字段：severity/vlm_confirm/…）
std::string mqtt_build_event_payload(
        const MQTTEvent &event
);

// 原始发布原语（mqtt 线程内调用）：任意 topic + payload + QoS1 + retained
bool mqtt_publish_raw_topic(
        const std::string &topic,
        const std::string &payload,
        int retained
);

// base64（远程快照回传；也用于报警缩略图）
std::string mqtt_base64_encode(
        const unsigned char *data,
        size_t len
);

// ---- 订阅 + 命令接收（[remote] 远程操控协议） ----
// 命令到达回调注册：paho 消息回调内仅快速转发（上层入队解析），
// 回调/接收线程绝不直调 VLM/DB
void mqtt_set_command_handler(
        void (*fn)(const char *payload, int len)
);

// 连接成功后设置 paho callbacks（connLost/msgArrived）。
// paho callbacks 绑定 client 句柄：重连重建句柄后必须重新调用
void mqtt_setup_callbacks();

// 订阅下行命令 topic（重连后必须重新订阅）
bool mqtt_subscribe_cmd();

// 轮询收取订阅消息（mqtt 线程空闲时调用；msgArrived 回调在
// receive 调用线程内同步触发，收到命令交给已注册 handler）
void mqtt_poll_incoming(int timeout_ms);


void mqtt_reconnect();


void mqtt_close();


#endif

