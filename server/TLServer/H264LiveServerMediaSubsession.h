#ifndef H264LIVESERVERMEDIASUBSESSION_H
#define H264LIVESERVERMEDIASUBSESSION_H

#include "config.h"
#include "SharedQueue.h"
#include "LiveSource.h"


class H264LiveServerMediaSubsession : public OnDemandServerMediaSubsession
{
    public:
        virtual FramedSource *createNewStreamSource(unsigned clientSessionId,unsigned &estBitrate);
        virtual RTPSink *createNewRTPSink(Groupsock *rtpGroupsock,unsigned char rtpPayloadTypeIfDynamic,FramedSource *inputSource);
        static H264LiveServerMediaSubsession *createNew(SharedQueue *queue, UsageEnvironment &env, Boolean reuseFirstSource, portNumBits initialPortNum = 6970, Boolean multiplexRTCPWithRTP = False);

    private:
        H264LiveServerMediaSubsession(SharedQueue * queue,UsageEnvironment & env, Boolean reuseFirstSource,portNumBits initialPortNum = 6970,Boolean multiplexRTCPWithRTP = False);
        ~H264LiveServerMediaSubsession();

        SharedQueue *shared_queue;

};

#endif
