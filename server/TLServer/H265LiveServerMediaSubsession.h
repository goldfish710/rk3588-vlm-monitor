#ifndef H265LIVESERVERMEDIASUBSESSION_H
#define H265LIVESERVERMEDIASUBSESSION_H

#include "config.h"
#include "SharedQueue.h"
#include "LiveSource.h"


class H265LiveServerMediaSubsession : public OnDemandServerMediaSubsession
{
    public:
        virtual FramedSource *createNewStreamSource(unsigned clientSessionId,unsigned &estBitrate);
        virtual RTPSink *createNewRTPSink(Groupsock *rtpGroupsock,unsigned char rtpPayloadTypeIfDynamic,FramedSource *inputSource);
        static H265LiveServerMediaSubsession *createNew(SharedQueue *queue, UsageEnvironment &env, Boolean reuseFirstSource, portNumBits initialPortNum = 6970, Boolean multiplexRTCPWithRTP = False);

    private:
        H265LiveServerMediaSubsession(SharedQueue * queue,UsageEnvironment & env, Boolean reuseFirstSource,portNumBits initialPortNum = 6970,Boolean multiplexRTCPWithRTP = False);
        ~H265LiveServerMediaSubsession();
       
        SharedQueue *shared_queue;
        
};

#endif
