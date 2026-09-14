#include "AACLiveSource.h"

AACLiveSource::AACLiveSource(UsageEnvironment &env, SharedQueue *_shared_queue) : FramedSource(env)
{
    shared_queue = _shared_queue;
    MppEventID = envir().taskScheduler().createEventTrigger(deliverFrame0);
    shared_queue->SetMppEventID(MppEventID);
    shared_queue->SetSource(this);
    offset = 0;
}

AACLiveSource::~AACLiveSource()
{
    envir().taskScheduler().deleteEventTrigger(MppEventID);
    if (shared_queue) {
        shared_queue->SetSource(nullptr);
        shared_queue->SetMppEventID(0);
    }
}

void AACLiveSource::doGetNextFrame()
{
    deliverFrame();
}

AACLiveSource *AACLiveSource::createNew(UsageEnvironment &env, SharedQueue *_shared_queue)
{
    return new AACLiveSource(env, _shared_queue);
}

void AACLiveSource::deliverFrame()
{
    if (!isCurrentlyAwaitingData())
        return;

    if (buffer.empty())
    {
        H265EncodePacket packet;
        if (!shared_queue || !shared_queue->pop_packet(packet))
        {
            return;
        }
        buffer = std::move(packet.data);
        timestamp = packet.timestamp;
        offset = 0;
    }

    unsigned sy = buffer.size() - offset;
    size_t size = std::min(sy, fMaxSize);

    fFrameSize = size;
    fPresentationTime = timestamp;
    fNumTruncatedBytes = 0;

    std::memcpy(fTo, buffer.data() + offset, size);
    offset += size;
    if (offset == buffer.size())
    {
        buffer.clear();
    }
    FramedSource::afterGetting(this);
}

void AACLiveSource::deliverFrame0(void *clientData)
{
    ((AACLiveSource *)clientData)->deliverFrame();
}
