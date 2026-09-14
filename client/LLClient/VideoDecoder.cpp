#include "VideoDecoder.h"
#include<QDebug>

VideoDecoder::VideoDecoder(const char *url, QObject *parent)
    : QThread(parent), rtsp_url(url), running(false)
{
}

VideoDecoder::~VideoDecoder()
{
    stop();
    wait(); // 等待线程退出
}

void VideoDecoder::stop()
{
    recording = false;
    running = false;
}

void VideoDecoder::startRecord(const QString &filePath)
{
    recordPath = filePath;
    recording = true;
}

void VideoDecoder::stopRecord()
{
    recording = false;
}

bool VideoDecoder::initFFmpeg()
{

    qDebug()<<"进入initFFmpeg";

    int result = avformat_open_input(&fmtCtx,rtsp_url,NULL,NULL);
    if(result<0)
    {
        return false;
    }

    //找到视频流
    avformat_find_stream_info(fmtCtx,NULL);

    for(unsigned i =0;i<fmtCtx->nb_streams;i++)
    {
        if(fmtCtx->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO)
        {
            video_stream=fmtCtx->streams[i];
            videoStreamIndex = i;
           // qDebug()<<"找到视频流";
        }
    }


    //创建硬件设备上下文 用cuvid硬件解码
    result = av_hwdevice_ctx_create(&hwDeviceCtx,AV_HWDEVICE_TYPE_CUDA,NULL,NULL,0);
    if(result != 0)
    {
        qDebug()<<"av_hwdevice_ctx_create error";
    }
    else
    {
        qDebug()<<"av_hwdevice_ctx_create 成功";
    }

    if(rtsp_url==MAIN_STREAM_URL)
    {
        decoder = avcodec_find_decoder_by_name("hevc_cuvid");
    }
    else if(rtsp_url==SUB_STREAM_URL)
    {
        decoder = avcodec_find_decoder_by_name("h264_cuvid");
    }

    if(decoder == nullptr)
    {
        return false;
    }


    //创建解码器上下文
    codecCtx = avcodec_alloc_context3(decoder);

    avcodec_parameters_to_context(codecCtx,video_stream->codecpar);

    //解码器上下文绑定硬件设备上下文
    codecCtx->hw_device_ctx=av_buffer_ref(hwDeviceCtx);

    result = avcodec_open2(codecCtx,decoder,NULL);
    if(result!=0)
    {
        return false;
    }

    //NV12->RGB24
    int width  = codecCtx->width;
    int height = codecCtx->height;
    swsCtx = sws_getContext(width, height, AV_PIX_FMT_NV12,
                            width, height, AV_PIX_FMT_RGB24,
                            SWS_BILINEAR, NULL, NULL, NULL);
    if(!swsCtx)
    {
        return false;
    }

    frame = av_frame_alloc();
    swFrame  = av_frame_alloc();
    rgbFrame = av_frame_alloc();

    int rgbSize = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1);
    rgbBuffer = (uint8_t *)av_malloc(rgbSize);
    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize,
                         rgbBuffer, AV_PIX_FMT_RGB24, width, height, 1);

    return true;
}

void VideoDecoder::cleanupFFmpeg()
{
    cleanupRecord();
    if(rgbBuffer)
    {
        av_free(rgbBuffer);
        rgbBuffer = nullptr;
    }
    if(rgbFrame)
    {
        av_frame_free(&rgbFrame);
    }
    if(swFrame)
    {
        av_frame_free(&swFrame);
    }
    if(frame)
    {
        av_frame_free(&frame);
    }
    if(swsCtx)
    {
        sws_freeContext(swsCtx);
        swsCtx = nullptr;
    }
    if(codecCtx)
    {
        avcodec_free_context(&codecCtx);
    }
    if(fmtCtx)
    {
        avformat_close_input(&fmtCtx);
    }
    if(hwDeviceCtx)
    {
        av_buffer_unref(&hwDeviceCtx);
    }
}

bool VideoDecoder::initRecord()
{
    //创建输出格式上下文(MP4)
    int ret = avformat_alloc_output_context2(&outFmtCtx, nullptr, nullptr,
                                              recordPath.toUtf8().constData());
    if(ret < 0 || !outFmtCtx)
    {
        qDebug() << "avformat_alloc_output_context2 error";
        return false;
    }

    //创建输出视频流，拷贝输入流的编码参数
    AVStream *outStream = avformat_new_stream(outFmtCtx, nullptr);
    if(!outStream)
    {
        qDebug() << "avformat_new_stream error";
        avformat_free_context(outFmtCtx);
        outFmtCtx = nullptr;
        return false;
    }
    outVideoIndex = outStream->index;
    avcodec_parameters_copy(outStream->codecpar, video_stream->codecpar);
    outStream->codecpar->codec_tag = 0;

    if(!(outFmtCtx->oformat->flags & AVFMT_NOFILE))
    {
        ret = avio_open(&outFmtCtx->pb, recordPath.toUtf8().constData(), AVIO_FLAG_WRITE);
        if(ret < 0)
        {
            qDebug() << "avio_open error";
            avformat_free_context(outFmtCtx);
            outFmtCtx = nullptr;
            return false;
        }
    }


    ret = avformat_write_header(outFmtCtx, nullptr);
    if(ret < 0)
    {
        qDebug() << "avformat_write_header error";
        avio_closep(&outFmtCtx->pb);
        avformat_free_context(outFmtCtx);
        outFmtCtx = nullptr;
        return false;
    }

    recordFileOpen = true;
    qDebug() << "录像开始:" << recordPath;
    return true;
}

void VideoDecoder::cleanupRecord()
{
    if(!recordFileOpen) return;

    av_write_trailer(outFmtCtx);

    if(!(outFmtCtx->oformat->flags & AVFMT_NOFILE))
    {
        avio_closep(&outFmtCtx->pb);
    }

    avformat_free_context(outFmtCtx);
    outFmtCtx = nullptr;
    outVideoIndex = -1;
    recordFileOpen = false;
    startDts = AV_NOPTS_VALUE;

    qDebug() << "录像结束:" << recordPath;
    emit recordFinished(recordPath);
}

void VideoDecoder::run()
{
    qDebug()<<"进入run";
    if (!initFFmpeg()) {
        qDebug()<<"initFFmpeg error";
        emit frameReady(QImage());//失败发一个空图片
        return;
    }
    qDebug()<<"initFFmpeg success";
    running = true;
    AVPacket *pkt = av_packet_alloc();

    while (running)
    {
        while(running && av_read_frame(fmtCtx,pkt)==0)
        {
            if(pkt->stream_index!=videoStreamIndex)
            {
                av_packet_unref(pkt);
                continue;
            }

            //录像逻辑：recording为true但文件未打开 → 等关键帧再初始化
            if(recording && !recordFileOpen)
            {
                if(pkt->flags & AV_PKT_FLAG_KEY)
                {
                    if(initRecord())
                    {
                        startDts = pkt->dts;
                    }
                }
            }
            //recording为false但文件还开着 → 停止录像
            if(!recording && recordFileOpen)
            {
                cleanupRecord();
            }
            //文件已打开 → 写入packet
            if(recordFileOpen)
            {
                AVPacket *outPkt = av_packet_clone(pkt);
                //减去起始偏移，让录像从0开始
                if(startDts != AV_NOPTS_VALUE)
                {
                    if(outPkt->pts != AV_NOPTS_VALUE)
                        outPkt->pts -= startDts;
                    if(outPkt->dts != AV_NOPTS_VALUE)
                        outPkt->dts -= startDts;
                }
                av_packet_rescale_ts(outPkt,
                                     video_stream->time_base,
                                     outFmtCtx->streams[outVideoIndex]->time_base);
                outPkt->stream_index = outVideoIndex;
                av_interleaved_write_frame(outFmtCtx, outPkt);
                av_packet_free(&outPkt);
            }

            int result=avcodec_send_packet(codecCtx,pkt);
            if(result != 0)
            {
                av_packet_unref(pkt);
                continue;
            }

            while(avcodec_receive_frame(codecCtx,frame)==0)
            {
                av_hwframe_transfer_data(swFrame,frame,0);//GPU->CPU

                //NV12->RGB24
                sws_scale(swsCtx,
                          swFrame->data, swFrame->linesize,
                          0, codecCtx->height,
                          rgbFrame->data, rgbFrame->linesize);

                //构造QImage发送到主线程显示
                QImage image(rgbFrame->data[0],
                             codecCtx->width, codecCtx->height,
                             rgbFrame->linesize[0],
                             QImage::Format_RGB888);
                emit frameReady(image.copy());
            }
            av_packet_unref(pkt);

        }
    }

    av_packet_free(&pkt);
    cleanupFFmpeg();
}
