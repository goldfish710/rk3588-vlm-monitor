#ifndef AAC_LIVE_SOURCE_H
#define AAC_LIVE_SOURCE_H

#include "config.h"
#include "SharedQueue.h"

// =====================================================
// RTSP 音频源（AAC）：照抄 LiveSource 分片交付模式。
// 一个包 = 一个完整 AAC AU（约 200B，远小于 fMaxSize，通常一次交付）。
// 复用 SharedQueue/H265EncodePacket 承载（data=AU, timestamp=帧时刻）。
// 无 framer：AAC AU 直接交付 MPEG4GenericRTPSink（AU header 自动生成）。
// =====================================================
class AACLiveSource : public FramedSource
{
    public:
        virtual ~AACLiveSource();
        static AACLiveSource *createNew(UsageEnvironment &env, SharedQueue *_shared_queue);
        static void deliverFrame0(void *clientData);

    private:
        virtual void doGetNextFrame();
        void deliverFrame();
        AACLiveSource(UsageEnvironment &env, SharedQueue *_shared_queue);

        EventTriggerId MppEventID;
        SharedQueue *shared_queue;
        std::vector<uint8_t> buffer;
        size_t offset;
        timeval timestamp{};
};

#endif
