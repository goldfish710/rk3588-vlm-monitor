#ifndef VIDEODECODER_H
#define VIDEODECODER_H

#include <QThread>
#include <QImage>
#include <atomic>
#include "const.h"
#include<QDebug>
class VideoDecoder : public QThread
{
    Q_OBJECT

public:
    VideoDecoder(const char *url, QObject *parent = nullptr);
    ~VideoDecoder();
    void stop();
    void startRecord(const QString &filePath);
    void stopRecord();

signals:
    void frameReady(const QImage &image);
    void recordFinished(const QString &filePath);

protected:
    void run() override;

private:
    const char *rtsp_url;
    std::atomic<bool> running;

    AVFormatContext *fmtCtx = nullptr;
    AVCodecContext  *codecCtx = nullptr;
    AVStream        *video_stream = nullptr;
    const AVCodec *decoder = nullptr;
    SwsContext      *swsCtx = nullptr;
    AVFrame         *frame = nullptr;
    AVFrame         *rgbFrame = nullptr;
    uint8_t         *rgbBuffer = nullptr;

    int videoStreamIndex = -1;

    AVBufferRef     *hwDeviceCtx = nullptr;
    AVFrame         *swFrame = nullptr;

    bool initFFmpeg();
    void cleanupFFmpeg();

    //录像相关
    std::atomic<bool> recording{false};
    AVFormatContext *outFmtCtx = nullptr;
    int outVideoIndex = -1;
    QString recordPath;
    bool recordFileOpen = false;
    int64_t startDts = AV_NOPTS_VALUE;

    bool initRecord();
    void cleanupRecord();
};

#endif // VIDEODECODER_H
