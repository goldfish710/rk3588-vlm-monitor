#include "AACLiveServerMediaSubsession.h"
#include "MPEG4GenericRTPSink.hh"

AACLiveServerMediaSubsession::AACLiveServerMediaSubsession(SharedQueue *queue, UsageEnvironment &env, Boolean reuseFirstSource,
                                                           const std::string& asc_hex, int sample_rate, int channels,
                                                           portNumBits initialPortNum, Boolean multiplexRTCPWithRTP)
    : OnDemandServerMediaSubsession(env, reuseFirstSource, initialPortNum, multiplexRTCPWithRTP),
      shared_queue(queue), asc_hex_(asc_hex), sample_rate_(sample_rate), channels_(channels)
{
}

AACLiveServerMediaSubsession::~AACLiveServerMediaSubsession()
{
}

FramedSource *AACLiveServerMediaSubsession::createNewStreamSource(unsigned /*clientSessionId*/, unsigned &estBitrate)
{
    estBitrate = 24;   // kbps（AAC-LC 16kHz mono 典型 24kbps）
    // 裸 AAC AU 直接交付，无 framer（AU header 由 MPEG4GenericRTPSink 自动生成）
    return AACLiveSource::createNew(envir(), shared_queue);
}

RTPSink *AACLiveServerMediaSubsession::createNewRTPSink(Groupsock *rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic,
                                                        FramedSource * /*inputSource*/)
{
    // 官方 ADTSAudioFileServerMediaSubsession 样板逐字对应：
    // MPEG4GenericRTPSink(env, gs, payloadType, 采样率, "audio", "AAC-hbr", configStr, 声道数)
    return MPEG4GenericRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic,
                                          (unsigned)sample_rate_, "audio", "AAC-hbr",
                                          asc_hex_.c_str(), (unsigned)channels_);
}

AACLiveServerMediaSubsession *AACLiveServerMediaSubsession::createNew(SharedQueue *queue, UsageEnvironment &env, Boolean reuseFirstSource,
                                                                      const std::string& asc_hex, int sample_rate, int channels,
                                                                      portNumBits initialPortNum, Boolean multiplexRTCPWithRTP)
{
    return new AACLiveServerMediaSubsession(queue, env, reuseFirstSource, asc_hex, sample_rate, channels,
                                            initialPortNum, multiplexRTCPWithRTP);
}
