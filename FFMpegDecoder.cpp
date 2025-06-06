#include "FFMpegDecoder.h"
#include <QtDebug>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

static const int OUT_SAMPLE_RATE = 44100;
static const int OUT_CHANNELS = 2;
static const AVSampleFormat OUT_SAMPLE_FMT = AV_SAMPLE_FMT_S16;

FFMpegDecoder::FFMpegDecoder(QObject *parent) : QObject(parent) {
  av_register_all();
}

FFMpegDecoder::~FFMpegDecoder() { stop(); }

void FFMpegDecoder::start(const QString &path) {
  stop(); // 如已有线程，先停止
  m_path = path;
  m_stop = false;
  m_pause = false;
  m_seeking = false;

  m_thread = std::thread(&FFMpegDecoder::decodeLoop, this);
}

void FFMpegDecoder::stop() {
  m_stop = true;
  m_cond.notify_all();
  if (m_thread.joinable())
    m_thread.join();
}

void FFMpegDecoder::seek(qint64 ms) {
  m_seekTarget = ms;
  m_seeking = true;
  m_cond.notify_all();
}

void FFMpegDecoder::togglePause() {
  m_pause = !m_pause;
  if (!m_pause)
    m_cond.notify_all();
}

void FFMpegDecoder::decodeLoop() {
  AVFormatContext *fmt_ctx = nullptr;
  if (avformat_open_input(&fmt_ctx, m_path.toUtf8().constData(), nullptr,
                          nullptr) < 0) {
    qWarning() << "Could not open input:" << m_path;
    return;
  }
  if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
    qWarning() << "Could not find stream info";
    avformat_close_input(&fmt_ctx);
    return;
  }

  // 找 video & audio 流
  int vid_idx = -1, aid_idx = -1;
  for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
    AVCodecParameters *p = fmt_ctx->streams[i]->codecpar;
    if (p->codec_type == AVMEDIA_TYPE_VIDEO && vid_idx < 0)
      vid_idx = i;
    if (p->codec_type == AVMEDIA_TYPE_AUDIO && aid_idx < 0)
      aid_idx = i;
  }
  // 视频解码器
  AVCodecContext *vctx = nullptr;
  SwsContext *sws_ctx = nullptr;
  int vwidth = 0, vheight = 0;
  AVRational vtime_base{0, 1};
  if (vid_idx >= 0) {
    AVCodec *vcodec =
        avcodec_find_decoder(fmt_ctx->streams[vid_idx]->codecpar->codec_id);
    if (!vcodec) {
      qWarning() << "Video decoder not found";
      avformat_close_input(&fmt_ctx);
      return;
    }
    vctx = avcodec_alloc_context3(vcodec);
    if (!vctx) {
      qWarning() << "Could not allocate video codec context";
      avformat_close_input(&fmt_ctx);
      return;
    }
    if (avcodec_parameters_to_context(vctx, fmt_ctx->streams[vid_idx]->codecpar) < 0) {
      qWarning() << "Could not copy video codec parameters";
      avcodec_free_context(&vctx);
      avformat_close_input(&fmt_ctx);
      return;
    }
    if (avcodec_open2(vctx, vcodec, nullptr) < 0) {
      qWarning() << "Could not open video codec";
      avcodec_free_context(&vctx);
      avformat_close_input(&fmt_ctx);
      return;
    }
    vwidth = vctx->width;
    vheight = vctx->height;
    vtime_base = fmt_ctx->streams[vid_idx]->time_base;
    // sws 用于转为 RGB24，明确设置色彩空间和范围
    int srcRange = 0; // mpeg标准范围
    int dstRange = 0;
    int brightness = 0, contrast = 1, saturation = 1;
    const int *inv_table = sws_getCoefficients(SWS_CS_ITU601);
    sws_ctx = sws_getCachedContext(
        nullptr,
        vwidth, vheight, vctx->pix_fmt,
        vwidth, vheight, AV_PIX_FMT_RGB24,
        SWS_BILINEAR | SWS_ACCURATE_RND,
        nullptr, nullptr, nullptr
    );
    if (sws_ctx) {
        sws_setColorspaceDetails(
            sws_ctx,
            inv_table, srcRange,
            sws_getCoefficients(SWS_CS_ITU601), dstRange,
            brightness, contrast, saturation
        );
    } else {
        qWarning() << "Could not initialize sws context";
    }
  }
  // 音频解码器
  AVCodecContext *actx = nullptr;
  SwrContext *swr_ctx = nullptr;
  AVRational atime_base{0, 1};
  if (aid_idx >= 0) {
    AVCodec *acodec =
        avcodec_find_decoder(fmt_ctx->streams[aid_idx]->codecpar->codec_id);
    if (!acodec) {
      qWarning() << "Audio decoder not found";
      if (vctx) avcodec_free_context(&vctx);
      avformat_close_input(&fmt_ctx);
      return;
    }
    actx = avcodec_alloc_context3(acodec);
    if (!actx) {
      qWarning() << "Could not allocate audio codec context";
      if (vctx) avcodec_free_context(&vctx);
      avformat_close_input(&fmt_ctx);
      return;
    }
    if (avcodec_parameters_to_context(actx, fmt_ctx->streams[aid_idx]->codecpar) < 0) {
      qWarning() << "Could not copy audio codec parameters";
      if (vctx) avcodec_free_context(&vctx);
      avcodec_free_context(&actx);
      avformat_close_input(&fmt_ctx);
      return;
    }
    if (avcodec_open2(actx, acodec, nullptr) < 0) {
      qWarning() << "Could not open audio codec";
      if (vctx) avcodec_free_context(&vctx);
      avcodec_free_context(&actx);
      avformat_close_input(&fmt_ctx);
      return;
    }
    atime_base = fmt_ctx->streams[aid_idx]->time_base;
    // swr 用于重采样到 S16/44100/2
    swr_ctx = swr_alloc();
    if (!swr_ctx) {
      qWarning() << "Could not allocate swr context";
    } else {
      av_opt_set_channel_layout(swr_ctx, "in_channel_layout",
                                actx->channel_layout, 0);
      av_opt_set_int(swr_ctx, "in_sample_rate", actx->sample_rate, 0);
      av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", actx->sample_fmt, 0);
      // 输出
      av_opt_set_channel_layout(swr_ctx, "out_channel_layout",
                                actx->channel_layout, 0);
      av_opt_set_int(swr_ctx, "out_sample_rate", OUT_SAMPLE_RATE, 0);
      av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", OUT_SAMPLE_FMT, 0);
      if (swr_init(swr_ctx) < 0) {
        qWarning() << "Could not initialize swr context";
        swr_free(&swr_ctx);
        swr_ctx = nullptr;
      }
    }
  }

  // 计算总时长
  qint64 duration_ms =
      fmt_ctx->duration >= 0 ? fmt_ctx->duration / (AV_TIME_BASE / 1000) : 0;
  emit durationChanged(duration_ms);

  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  // 视频输出 buffer
  int rgb_stride = vwidth * 3;
  uint8_t *rgb_buf = nullptr;
  if (vwidth && vheight) {
    int numBytes =
        av_image_get_buffer_size(AV_PIX_FMT_RGB24, vwidth, vheight, 1);
    rgb_buf = (uint8_t *)av_malloc(numBytes);
  }

  // 主循环
  while (!m_stop) {
    // pause
    if (m_pause) {
      std::unique_lock<std::mutex> lk(m_mutex);
      m_cond.wait(lk, [&] { return m_stop || !m_pause; });
      if (m_stop)
        break;
    }
    // seek
    if (m_seeking) {
      int64_t ts = m_seekTarget * (AV_TIME_BASE / 1000);
      av_seek_frame(fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
      if (vctx)
        avcodec_flush_buffers(vctx);
      if (actx)
        avcodec_flush_buffers(actx);
      m_seeking = false;
    }
    if (av_read_frame(fmt_ctx, pkt) < 0)
      break;

    // 视频包
    if (pkt->stream_index == vid_idx) {
      if (!vctx || !sws_ctx) {
        av_packet_unref(pkt);
        continue;
      }
      avcodec_send_packet(vctx, pkt);
      while (avcodec_receive_frame(vctx, frame) == 0) {
        // 转为 RGB24
        uint8_t *dst[1] = {rgb_buf};
        int dst_linesize[1] = {rgb_stride};
        sws_scale(sws_ctx, frame->data, frame->linesize, 0, vheight, dst, dst_linesize);
        QImage img(rgb_buf, vwidth, vheight, rgb_stride, QImage::Format_RGB888);
        emit frameReady(img.copy()); // 复制数据，避免线程安全问题

        // 计算 pts 并发送
        qint64 ms = frame->best_effort_timestamp * vtime_base.num * 1000LL /
                    vtime_base.den;
        emit positionChanged(ms);
      }
    }
    // 音频包
    else if (pkt->stream_index == aid_idx) {
      if (!actx || !swr_ctx) {
        av_packet_unref(pkt);
        continue;
      }
      avcodec_send_packet(actx, pkt);
      while (avcodec_receive_frame(actx, frame) == 0) {
        // 重采样
        int out_nb = av_rescale_rnd(
            swr_get_delay(swr_ctx, actx->sample_rate) + frame->nb_samples,
            OUT_SAMPLE_RATE, actx->sample_rate, AV_ROUND_UP);
        // 分配 buffer
        uint8_t **out_buf = nullptr;
        av_samples_alloc_array_and_samples(&out_buf, nullptr, OUT_CHANNELS,
                                           out_nb, OUT_SAMPLE_FMT, 0);
        int converted =
            swr_convert(swr_ctx, out_buf, out_nb, (const uint8_t **)frame->data,
                        frame->nb_samples);

        int data_size = av_samples_get_buffer_size(
            nullptr, OUT_CHANNELS, converted, OUT_SAMPLE_FMT, 1);
        QByteArray pcm((const char *)out_buf[0], data_size);
        emit audioReady(pcm);

        // 计算 pts 并发送
        qint64 ms = frame->pts != AV_NOPTS_VALUE
                        ? frame->pts * atime_base.num * 1000LL / atime_base.den
                        : 0;
        emit positionChanged(ms);

        av_freep(&out_buf[0]);
        av_freep(&out_buf);
      }
    }
    av_packet_unref(pkt);
  }

  // cleanup
  if (rgb_buf)
    av_free(rgb_buf);
  av_frame_free(&frame);
  av_packet_free(&pkt);
  if (sws_ctx)
    sws_freeContext(sws_ctx);
  if (vctx)
    avcodec_free_context(&vctx);
  if (swr_ctx)
    swr_free(&swr_ctx);
  if (actx)
    avcodec_free_context(&actx);
  avformat_close_input(&fmt_ctx);
}