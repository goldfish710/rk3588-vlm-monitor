#include "H264LiveServerMediaSubsession.h"

H264LiveServerMediaSubsession::H264LiveServerMediaSubsession(SharedQueue * queue,UsageEnvironment & env, Boolean reuseFirstSource, portNumBits initialPortNum, Boolean multiplexRTCPWithRTP) : OnDemandServerMediaSubsession(env, reuseFirstSource, initialPortNum, multiplexRTCPWithRTP)
{
    shared_queue = queue;
}

H264LiveServerMediaSubsession::~H264LiveServerMediaSubsession()
{

}

FramedSource * H264LiveServerMediaSubsession::createNewStreamSource(unsigned clientSessionId, unsigned &estBitrate)
{
    estBitrate = g_config.sub_stream.bitrate / 1000;
    LiveSource *source = LiveSource::createNew(envir(), shared_queue);
    return H264VideoStreamFramer::createNew(envir(),source);
}

RTPSink *H264LiveServerMediaSubsession::createNewRTPSink(Groupsock *rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic, FramedSource *inputSource)
{
    OutPacketBuffer::maxSize = g_config.sub_stream.rtp_out_buf_max;
    return H264VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic, shared_queue->GetSPS().data(), shared_queue->GetSPS().size(), shared_queue->GetPPS().data(), shared_queue->GetPPS().size());
}

H264LiveServerMediaSubsession *H264LiveServerMediaSubsession::createNew(SharedQueue *queue, UsageEnvironment &env, Boolean reuseFirstSource, portNumBits initialPortNum, Boolean multiplexRTCPWithRTP)
{
    return new H264LiveServerMediaSubsession(queue,env, reuseFirstSource, initialPortNum, multiplexRTCPWithRTP);
}
