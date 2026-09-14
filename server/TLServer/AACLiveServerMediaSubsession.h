#ifndef AAC_LIVE_SERVER_MEDIA_SUBSESSION_H
#define AAC_LIVE_SERVER_MEDIA_SUBSESSION_H

#include "config.h"
#include "SharedQueue.h"
#include "AACLiveSource.h"

// =====================================================
// RTSP 音频子会话（AAC-hbr，RFC 3640）
// 接线参照 live555 官方 ADTSAudioFileServerMediaSubsession 样板：
//   source = AACLiveSource（裸 AAC AU，无 framer）
//   sink   = MPEG4GenericRTPSink(AAC-hbr, configStr=ASC, mono)
// =====================================================
class AACLiveServerMediaSubsession : public OnDemandServerMediaSubsession
{
    public:
        virtual FramedSource *createNewStreamSource(unsigned clientSessionId, unsigned &estBitrate);
        virtual RTPSink *createNewRTPSink(Groupsock *rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic, FramedSource *inputSource);
        static AACLiveServerMediaSubsession *createNew(SharedQueue *queue, UsageEnvironment &env, Boolean reuseFirstSource,
                                                       const std::string& asc_hex, int sample_rate, int channels,
                                                       portNumBits initialPortNum = 6970, Boolean multiplexRTCPWithRTP = False);

    private:
        AACLiveServerMediaSubsession(SharedQueue *queue, UsageEnvironment &env, Boolean reuseFirstSource,
                                     const std::string& asc_hex, int sample_rate, int channels,
                                     portNumBits initialPortNum = 6970, Boolean multiplexRTCPWithRTP = False);
        ~AACLiveServerMediaSubsession();

        SharedQueue *shared_queue;
        std::string asc_hex_;       // AudioSpecificConfig 十六进制串（如 "1408"）
        int sample_rate_ = 16000;
        int channels_ = 1;
};

#endif
