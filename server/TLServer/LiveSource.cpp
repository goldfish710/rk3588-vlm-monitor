#include "LiveSource.h"

LiveSource::LiveSource(UsageEnvironment &env, SharedQueue *_shared_queue) : FramedSource(env)
{
    shared_queue = _shared_queue;
    MppEventID = envir().taskScheduler().createEventTrigger(deliverFrame0);
    shared_queue->SetMppEventID(MppEventID);
    shared_queue->SetSource(this);
    offset = 0;
}

LiveSource::~LiveSource()
{
    envir().taskScheduler().deleteEventTrigger(MppEventID);
    if (shared_queue) {
        shared_queue->SetSource(nullptr);
        shared_queue->SetMppEventID(0);   // 可选，避免再触发
    }
}

void LiveSource::doGetNextFrame()
{
    deliverFrame();
}

LiveSource *LiveSource::createNew(UsageEnvironment &env, SharedQueue *_shared_queue)
{
    return new LiveSource(env, _shared_queue);
}

void LiveSource::deliverFrame()
{
    if (!isCurrentlyAwaitingData())
        return;

    if(buffer.empty())
    {
        H265EncodePacket packet;
        // 无法从共享队列中取得packet
        if (!shared_queue || !shared_queue->pop_packet(packet))
        {
            return;
        }
        buffer = std::move(packet.data);
        timestamp = packet.timestamp;
        offset = 0;
    }

    unsigned sy = buffer.size() - offset;//内部buffer剩余量
    size_t size = std::min(sy, fMaxSize);

    //提交给下游的三个变量
    fFrameSize = size;
    fPresentationTime = timestamp;
    fNumTruncatedBytes = 0;

    std::memcpy(fTo, buffer.data()+offset, size);
    offset += size;
    if(offset == buffer.size())
    {
        buffer.clear();
    }
    FramedSource::afterGetting(this);
}


void LiveSource::deliverFrame0(void *clientData)
{
    ((LiveSource *)clientData)->deliverFrame();
}

EventTriggerId LiveSource::GetMppEventID()
{
    return MppEventID;
}
