#ifndef LIVESOURCE_H
#define LIVESOURCE_H

#include "config.h"
#include "SharedQueue.h"



class LiveSource : public FramedSource
{
    public:
        virtual ~LiveSource();
        static LiveSource *createNew(UsageEnvironment &env, SharedQueue *_shared_queue);
        static void deliverFrame0(void *clientData);
        EventTriggerId GetMppEventID();

    private:
        virtual void doGetNextFrame();
        void deliverFrame();
        LiveSource(UsageEnvironment &env, SharedQueue *_shared_queue);

        EventTriggerId MppEventID;
        SharedQueue *shared_queue;
        std::vector<uint8_t> buffer; // 内部维护一个buffer承接共享队列中的packet
        size_t offset;//维护buffer的偏移量
        timeval timestamp{};
};

#endif
