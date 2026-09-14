#ifndef SHAREDQUEUE_H
#define SHAREDQUEUE_H

#include "config.h"

class SharedQueue
{
    public:
        SharedQueue(EventTriggerId MppEventID_ = 0, void *source_ = NULL);
        ~SharedQueue();
        void SetMppEventID(EventTriggerId MppEventID_);
        void SetSource(void *source);
        void *GetSource();
        bool push_packet(const H265EncodePacket &packet);
        bool pop_packet(H265EncodePacket& packet);
        EventTriggerId GetMppEventID();
        void SetVPS(uint8_t *data, size_t len);
        void SetSPS(uint8_t *data, size_t len);
        void SetPPS(uint8_t *data, size_t len);
        const std::vector<uint8_t>& GetVPS();
        const std::vector<uint8_t>& GetSPS();
        const std::vector<uint8_t>& GetPPS();
        
        // 【新增】获取当前队列长度（线程安全）
        size_t size() {
            std::lock_guard<std::mutex> lock(mutex_);
            return queue_.size();
        }

    private:
        std::atomic<EventTriggerId> MppEventID;
        std::atomic<void*> Source;
        std::deque<H265EncodePacket> queue_;
        std::mutex mutex_;
        size_t max_size_;
        std::vector<uint8_t> vps;
        std::vector<uint8_t> pps;
        std::vector<uint8_t> sps;
};

#endif