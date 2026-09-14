#include "SharedQueue.h"

SharedQueue::SharedQueue(EventTriggerId MppEventID_, void *source_) : max_size_(4)
{
    MppEventID = MppEventID_;
    Source = source_;
}

SharedQueue::~SharedQueue()
{
    
}

bool SharedQueue::push_packet(const H265EncodePacket &packet)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(queue_.size()>=max_size_)
    {
        queue_.pop_front();//丢弃旧帧
        //std::cout << "共享队列满帧，丢弃旧帧!!!!!!!!!!!!!!" << std::endl;
    }

    queue_.push_back(packet);
    return true;
}

bool SharedQueue::pop_packet(H265EncodePacket &packet)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(queue_.size()==0)
    {
        return false;
    }

    packet = queue_.front();
    queue_.pop_front();
    return true;
}

void SharedQueue::SetMppEventID(EventTriggerId MppEventID_)
{
    MppEventID = MppEventID_;
}

EventTriggerId SharedQueue::GetMppEventID()
{
    return MppEventID;
}

void SharedQueue::SetSource(void *source)
{
    Source = source;
}

void* SharedQueue::GetSource()
{
    return Source;
}

void SharedQueue::SetVPS(uint8_t *data, size_t len)
{
    vps.assign(data, data + len);
}

void SharedQueue::SetSPS(uint8_t *data, size_t len)
{
    sps.assign(data, data + len);
}

void SharedQueue::SetPPS(uint8_t *data, size_t len)
{
    pps.assign(data, data + len);
}

const std::vector<uint8_t>& SharedQueue::GetVPS()
{
    return vps;
}

const std::vector<uint8_t>& SharedQueue::GetSPS()
{
    return sps;
}

const std::vector<uint8_t>& SharedQueue::GetPPS()
{
    return pps;
}
