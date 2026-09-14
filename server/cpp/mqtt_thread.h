#ifndef MQTT_THREAD_H
#define MQTT_THREAD_H


#include <string>
#include <vector>
#include <ctime>
#include <cstdint>


struct MQTTEvent
{

    //事件类型
    std::string event;


    //设备编号
    std::string device_id;


    //置信度
    float confidence;


    //事件时间
    time_t timestamp;


    //未来扩展

    //图片路径
    std::string image;


    //视频地址
    std::string video;


    //现场图片 JPEG 字节流（缩略图），由 mqtt_client 转 base64 后携带在消息里
    std::vector<unsigned char> image_jpeg;

    // ---- VLM 复核/分级字段（可选：空/默认值不序列化，老订阅端零感知） ----
    std::string severity;      // urgent=真跌倒 attention=未确认 cleared=VLM 排除
    std::string summary;       // 状态摘要文本（时间线消息用，阶段B）
    int vlm_confirm = -1;      // -1=未复核 0=排除 1=真跌倒 2=无法确认
    std::string vlm_reply;     // VLM 结论原因/摘要原文
    uint64_t req_id = 0;       // 远程命令应答关联 id（阶段C，0=不携带）

};



// 原始消息推送（topic/payload 直接指定，B/C 阶段时间线/应答/状态通道共用）
// retained=true 时 broker 保留最后一条（设备状态类消息用）
void mqtt_push_raw(
        const std::string &topic,
        const std::string &payload,
        bool retained
);


void start_mqtt_thread();


void stop_mqtt_thread();



void mqtt_push_event(
        const MQTTEvent &event
);


#endif