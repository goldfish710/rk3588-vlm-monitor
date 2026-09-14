#include "mqtt_client.h"
#include "config.h"
#include <MQTTClient.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static MQTTClient client = NULL;
static bool mqtt_connected = false;
static void (*g_cmd_handler)(const char *, int) = nullptr;   // 命令分发（TLmain 注册 → ChatEngine）

// 连接丢失回调：仅置标志（mqtt 线程的下次检查触发重建+重订阅）
static void connLost(void *context, char *cause)
{
    (void)context;
    printf("MQTT connection lost: %s\n", cause ? cause : "");
    mqtt_connected = false;
}

void mqtt_set_command_handler(void (*fn)(const char *, int))
{
    g_cmd_handler = fn;
}

// =====================================================
// Base64 编码（MQTT 是文本协议，二进制图片需转文本传输）
// 每 3 字节(24bit) → 4 个 6bit → 查表映射为可打印字符
// =====================================================
static const char B64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const unsigned char* data, size_t len)
{
    std::string out;
    out.reserve((len + 2) / 3 * 4);

    size_t i = 0;
    while (i + 3 <= len)
    {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(B64_TABLE[(v >> 18) & 0x3F]);
        out.push_back(B64_TABLE[(v >> 12) & 0x3F]);
        out.push_back(B64_TABLE[(v >> 6) & 0x3F]);
        out.push_back(B64_TABLE[v & 0x3F]);
        i += 3;
    }

    // 尾部不足 3 字节：补 0 编码，末尾用 '=' 填充
    if (len - i == 1)
    {
        uint32_t v = data[i] << 16;
        out.push_back(B64_TABLE[(v >> 18) & 0x3F]);
        out.push_back(B64_TABLE[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    }
    else if (len - i == 2)
    {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
        out.push_back(B64_TABLE[(v >> 18) & 0x3F]);
        out.push_back(B64_TABLE[(v >> 12) & 0x3F]);
        out.push_back(B64_TABLE[(v >> 6) & 0x3F]);
        out.push_back('=');
    }

    return out;
}

static void mqtt_destroy() {
    if (client) {
        if (MQTTClient_isConnected(client)) {
            MQTTClient_disconnect(client, 1000);
        }
        MQTTClient_destroy(&client);
        client = NULL;
    }
    mqtt_connected = false;
}

bool mqtt_connect() {
    if (client != NULL) {
        mqtt_destroy();
    }

    const MqttConfig& mc = g_config.mqtt;
    int rc = MQTTClient_create(&client,
                               mc.broker_url.c_str(),
                               mc.client_id.c_str(),
                               MQTTCLIENT_PERSISTENCE_NONE,
                               NULL);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("MQTTClient_create failed, rc=%d\n", rc);
        client = NULL;
        return false;
    }

    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    conn_opts.username = mc.username.c_str();
    conn_opts.password = mc.password.c_str();
    conn_opts.keepAliveInterval = mc.keepalive;
    conn_opts.cleansession = 0;
    conn_opts.connectTimeout = mc.connect_timeout;

    printf("MQTT connecting to %s ...\n", mc.broker_url.c_str());
    rc = MQTTClient_connect(client, &conn_opts);
    if (rc == MQTTCLIENT_SUCCESS) {
        mqtt_connected = true;
        printf("MQTT connected\n");
        return true;
    }

    mqtt_connected = false;
    printf("MQTT connect failed, rc=%d\n", rc);
    MQTTClient_destroy(&client);
    client = NULL;
    return false;
}

void mqtt_reconnect() {
    printf("MQTT rebuilding connection...\n");
    mqtt_destroy();
    mqtt_connect();
}

bool mqtt_is_connected() {
    return mqtt_connected && client && MQTTClient_isConnected(client);
}

// JSON 字符串转义（文本字段可能含引号/反斜杠/控制符）
static std::string json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
            else out += c;
        }
    }
    return out;
}

std::string mqtt_build_event_payload(const MQTTEvent &event)
{
    // ---------- 组装 JSON 字符串（动态长度，可携带 base64 图片） ----------
    char timestamp[64];
    struct tm *tm_info = localtime(&event.timestamp);
    snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d",
             tm_info->tm_year+1900, tm_info->tm_mon+1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

    char conf_buf[16];
    snprintf(conf_buf, sizeof(conf_buf), "%.2f", event.confidence);

    std::string payload;
    payload.reserve(1024);
    payload += "{\"event\":\"";
    payload += json_escape(event.event);
    payload += "\",\"device_id\":\"";
    payload += json_escape(event.device_id);
    payload += "\",\"timestamp\":\"";
    payload += timestamp;
    payload += "\",\"confidence\":";
    payload += conf_buf;
    payload += ",\"image\":\"";
    payload += json_escape(event.image);
    payload += "\",\"video\":\"";
    payload += json_escape(event.video);
    payload += "\"";

    // 现场图片：缩略图 JPEG → base64 文本塞进 JSON（可配置关闭）
    if (g_config.mqtt.include_image_base64 && !event.image_jpeg.empty())
    {
        std::string b64 = base64_encode(event.image_jpeg.data(), event.image_jpeg.size());
        payload += ",\"image_base64\":\"";
        payload += b64;
        payload += "\"";
        printf("MQTT attach image: %zu bytes jpeg -> %zu bytes base64\n",
               event.image_jpeg.size(), b64.size());
    }

    // ---- VLM 复核/分级可选字段（非空/非默认才序列化，老订阅端零感知） ----
    if (!event.severity.empty()) {
        payload += ",\"severity\":\"";
        payload += json_escape(event.severity);
        payload += "\"";
    }
    if (!event.summary.empty()) {
        payload += ",\"summary\":\"";
        payload += json_escape(event.summary);
        payload += "\"";
    }
    if (event.vlm_confirm >= 0) {
        payload += ",\"vlm_confirm\":";
        payload += std::to_string(event.vlm_confirm);
    }
    if (!event.vlm_reply.empty()) {
        payload += ",\"vlm_reply\":\"";
        payload += json_escape(event.vlm_reply);
        payload += "\"";
    }
    if (event.req_id != 0) {
        payload += ",\"req_id\":";
        payload += std::to_string(event.req_id);
    }

    payload += "}";
    return payload;
}

bool mqtt_publish_raw_topic(const std::string &topic, const std::string &payload, int retained)
{
    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload = (void*)payload.c_str();
    msg.payloadlen = (int)payload.size();
    msg.qos = 1;
    msg.retained = retained;

    MQTTClient_deliveryToken token;
    int rc = MQTTClient_publishMessage(client, topic.c_str(), &msg, &token);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("MQTT publishMessage failed, rc=%d\n", rc);
        return false;
    }

    // 等待 QoS1 ACK 确认，超时也视为失败。
    // 超时值可配（默认 5s）：原来写死 1s，在公网 + 大 payload 下等不到 PUBACK
    // → 被判失败 → mqtt 线程回队重发 → 对端收到重复应答（快照会存成两张图）
    rc = MQTTClient_waitForCompletion(client, token, g_config.mqtt.publish_ack_timeout_ms);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("MQTT waitForCompletion timeout/failed (%dms), rc=%d\n",
               g_config.mqtt.publish_ack_timeout_ms, rc);
        return false;
    }

    printf("MQTT sent (%d bytes) to %s:\n%s\n", (int)payload.size(), topic.c_str(),
           payload.size() > 512 ? "[payload too long, truncated]" : payload.c_str());
    return true;
}

void mqtt_close() {
    if (client) {
        if (mqtt_connected) {
            MQTTClient_disconnect(client, 1000);
        }
        MQTTClient_destroy(&client);
        client = NULL;   // 修复：销毁后置空，避免悬空指针被再次 destroy
    }
    mqtt_connected = false;
}

std::string mqtt_base64_encode(const unsigned char *data, size_t len)
{
    return base64_encode(data, len);
}

// ==================== 订阅 + 命令接收（远程操控协议） ====================
void mqtt_setup_callbacks()
{
    if (!client) return;
    // 只设 connLost（连接异常感知）；消息接收不依赖 msgArrived 回调——
    // paho 1.3 同步客户端经 MQTTClient_receive 拉取时回调投递路径实测不可靠
    // （订阅成功但命令静默丢失），poll_incoming 里直接处理消息本体
    int rc = MQTTClient_setCallbacks(client, NULL, connLost, NULL, NULL);
    if (rc != MQTTCLIENT_SUCCESS)
        printf("MQTT setCallbacks failed, rc=%d\n", rc);
}

bool mqtt_subscribe_cmd()
{
    if (!client || !mqtt_connected) return false;
    // 总开关：subscribe_enable（[mqtt]）+ remote_enable（[vlm_pipeline]）任一关则不订阅
    if (!g_config.mqtt.subscribe_enable || !g_config.chat.vlm_pipeline.remote_enable)
        return false;
    int rc = MQTTClient_subscribe(client, g_config.mqtt.cmd_topic.c_str(), 1);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("MQTT subscribe failed(%s), rc=%d\n", g_config.mqtt.cmd_topic.c_str(), rc);
        return false;
    }
    printf("MQTT subscribed: %s\n", g_config.mqtt.cmd_topic.c_str());
    return true;
}

void mqtt_poll_incoming(int timeout_ms)
{
    if (!client || !mqtt_connected) return;
    char *topicName = NULL;
    int topicLen = 0;
    MQTTClient_message *message = NULL;
    int rc = MQTTClient_receive(client, &topicName, &topicLen, &message, timeout_ms);
    // 标准同步客户端模式：消息本体在 receive 返回处直接处理（handler 仅入队，
    // 解析与执行在 chat 线程），不依赖 msgArrived 回调
    if (rc == MQTTCLIENT_SUCCESS && message) {
        if (g_cmd_handler && message->payload && message->payloadlen > 0)
            g_cmd_handler((const char *)message->payload, (int)message->payloadlen);
        MQTTClient_freeMessage(&message);
    }
    if (topicName) MQTTClient_free(topicName);
}
