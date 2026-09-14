#include "H265LiveServerMediaSubsession.h"

H265LiveServerMediaSubsession::H265LiveServerMediaSubsession(SharedQueue * queue,UsageEnvironment & env, Boolean reuseFirstSource, portNumBits initialPortNum, Boolean multiplexRTCPWithRTP) : OnDemandServerMediaSubsession(env, reuseFirstSource, initialPortNum, multiplexRTCPWithRTP)
{
    shared_queue = queue;
}

H265LiveServerMediaSubsession::~H265LiveServerMediaSubsession()
{

}

FramedSource * H265LiveServerMediaSubsession::createNewStreamSource(unsigned clientSessionId, unsigned &estBitrate)
{
    estBitrate = g_config.main_stream.bitrate / 1000;
    LiveSource *source = LiveSource::createNew(envir(), shared_queue);
    return H265VideoStreamFramer::createNew(envir(),source);
}

RTPSink *H265LiveServerMediaSubsession::createNewRTPSink(Groupsock *rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic, FramedSource *inputSource)
{
    OutPacketBuffer::maxSize = g_config.main_stream.rtp_out_buf_max;
    return H265VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic, shared_queue->GetVPS().data(), shared_queue->GetVPS().size(), shared_queue->GetSPS().data(), shared_queue->GetSPS().size(), shared_queue->GetPPS().data(), shared_queue->GetPPS().size());
   
}

H265LiveServerMediaSubsession *H265LiveServerMediaSubsession::createNew(SharedQueue *queue, UsageEnvironment &env, Boolean reuseFirstSource, portNumBits initialPortNum, Boolean multiplexRTCPWithRTP)
{
    return new H265LiveServerMediaSubsession(queue,env, reuseFirstSource, initialPortNum, multiplexRTCPWithRTP);
}

