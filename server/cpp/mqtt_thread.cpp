#include "mqtt_thread.h"
#include "mqtt_client.h"
#include "config.h"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>
#include <stdio.h>

// 队列元素：最终发布原语（topic+payload+qos+retained）。
// 报警/时间线/应答/状态各通道统一走这条队列，由 mqtt 线程串行发布
struct MqttRawMsg {
    std::string topic;
    std::string payload;
    int retained = 1;
};

static std::queue<MqttRawMsg> mqtt_queue;
static std::mutex mqtt_mutex;
static std::condition_variable mqtt_cv;
static std::thread mqtt_thread;
static std::atomic<bool> mqtt_running(false);
static const size_t MAX_QUEUE_SIZE = 100;

static void mqtt_enqueue(MqttRawMsg &&msg, const char *tag)
{
    {
        std::lock_guard<std::mutex> lock(mqtt_mutex);
        if (mqtt_queue.size() >= MAX_QUEUE_SIZE) {
            mqtt_queue.pop();   // 丢弃最旧消息
            printf("[MQTT-Q] 队列满(%zu)，丢弃最旧消息(%s)\n", MAX_QUEUE_SIZE, tag);
        }
        mqtt_queue.push(std::move(msg));
        printf("[MQTT-Q] %s入队, 当前队列长度=%zu\n", tag, mqtt_queue.size());
    }
    mqtt_cv.notify_one();
}

// 报警事件：组装 payload（在调用线程做 JSON/base64，mqtt 线程只负责发）
void mqtt_push_event(const MQTTEvent &event)
{
    MqttRawMsg msg;
    msg.topic = g_config.mqtt.topic;
    msg.payload = mqtt_build_event_payload(event);
    msg.retained = 1;
    mqtt_enqueue(std::move(msg), "事件");
}

// 原始消息推送（B/C 阶段时间线/应答/状态通道；报警 retained，时间线/应答不保留）
void mqtt_push_raw(const std::string &topic, const std::string &payload, bool retained)
{
    MqttRawMsg msg;
    msg.topic = topic;
    msg.payload = payload;
    msg.retained = retained ? 1 : 0;
    mqtt_enqueue(std::move(msg), "原始");
}
static void mqtt_loop() {
    printf("MQTT thread started\n");

    bool connected = false;
    int retry_delay = 1;
    const int MAX_RETRY_DELAY = 60;
    int consecutive_publish_failures = 0;

    while (mqtt_running) {
        // ---------- 连接管理 ----------
        if (!connected) {
            printf("MQTT attempting to connect...\n");
            connected = mqtt_connect();
            if (connected) {
                printf("MQTT connected successfully\n");
                retry_delay = 1;
                consecutive_publish_failures = 0;
                // 重建句柄后必须重设 callbacks + 重新订阅（paho 回调/订阅绑定 client 句柄）
                mqtt_setup_callbacks();
                mqtt_subscribe_cmd();
                // 发布设备在线状态（retained：后来者也能读到最近状态）
                {
                    char ts[32];
                    time_t now = time(NULL);
                    struct tm tmv;
                    localtime_r(&now, &tmv);
                    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
                    std::string payload = "{\"state\":\"online\",\"ts\":\"";
                    payload += ts;
                    payload += "\",\"device_id\":\"";
                    payload += g_config.event.device_id;
                    payload += "\"}";
                    mqtt_publish_raw_topic(g_config.mqtt.state_topic, payload, 1);
                }
            } else {
                for (int i = 0; i < retry_delay && mqtt_running; ++i)
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                if (retry_delay < MAX_RETRY_DELAY)
                    retry_delay <<= 1;
                continue;
            }
        }

        // ---------- 取出消息 ----------
        MqttRawMsg msg;
        bool has_msg = false;
        {
            std::unique_lock<std::mutex> lock(mqtt_mutex);
            if (mqtt_queue.empty()) {
                mqtt_cv.wait_for(lock, std::chrono::seconds(1));
            }
            if (!mqtt_queue.empty()) {
                msg = std::move(mqtt_queue.front());
                mqtt_queue.pop();
                has_msg = true;
            }
        }

        // ---------- 发布消息 ----------
        if (has_msg) {
            if (!mqtt_is_connected()) {
                connected = false;
                {
                    std::lock_guard<std::mutex> lock(mqtt_mutex);
                    mqtt_queue.push(std::move(msg));
                }
                continue;
            }

            bool success = mqtt_publish_raw_topic(msg.topic, msg.payload, msg.retained);
            if (!success) {
                printf("MQTT send failed, will retry later\n");
                consecutive_publish_failures++;

                // 放回队列
                {
                    std::lock_guard<std::mutex> lock(mqtt_mutex);
                    mqtt_queue.push(std::move(msg));
                }

                if (!mqtt_is_connected()) {
                    connected = false;
                    printf("Connection lost, mark for reconnect\n");
                } else {
                    printf("Publish ACK timeout, force reconnect\n");
                    mqtt_close();
                    connected = false;
                }

                // 指数退避避免紧循环
                int sleep_sec = 1;
                for (int i = 0; i < consecutive_publish_failures && sleep_sec < 30; ++i)
                    sleep_sec *= 2;
                if (sleep_sec > 30) sleep_sec = 30;
                printf("Backoff %d seconds before retry\n", sleep_sec);
                std::this_thread::sleep_for(std::chrono::seconds(sleep_sec));
            } else {
                consecutive_publish_failures = 0;
            }
        } else {
            // 空闲时收取订阅命令（msgArrived 回调内转发给上层入队）
            mqtt_poll_incoming(200);
            // 检查连接健康
            if (connected && !mqtt_is_connected()) {
                connected = false;
                printf("MQTT connection lost (detected by check)\n");
            }
        }
    }

    // 退出前尽力发布 offline（retained，best effort 不等 ACK）
    if (mqtt_is_connected()) {
        char ts[32];
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
        std::string payload = "{\"state\":\"offline\",\"ts\":\"";
        payload += ts;
        payload += "\",\"device_id\":\"";
        payload += g_config.event.device_id;
        payload += "\"}";
        mqtt_publish_raw_topic(g_config.mqtt.state_topic, payload, 1);
    }

    mqtt_close();
    printf("MQTT thread stopped\n");
}

void start_mqtt_thread() {
    mqtt_running = true;
    mqtt_thread = std::thread(mqtt_loop);
}

void stop_mqtt_thread() {
    mqtt_running = false;
    mqtt_cv.notify_all();
    if (mqtt_thread.joinable()) {
        mqtt_thread.join();
    }
}