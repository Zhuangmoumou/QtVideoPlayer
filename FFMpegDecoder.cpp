#include "FFMpegDecoder.h"
#include <QtDebug>
#include <chrono>

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

  m_videoThread = std::thread(&FFMpegDecoder::videoDecodeLoop, this);
  m_audioThread = std::thread(&FFMpegDecoder::audioDecodeLoop, this);
}

void FFMpegDecoder::stop() {
  m_stop = true;
  m_cond.notify_all();
  if (m_videoThread.joinable())
    m_videoThread.join();
  if (m_audioThread.joinable())
    m_audioThread.join();
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

bool FFMpegDecoder::isPaused() const {
  return m_pause;
}

// 拆分后的视频解码循环
void FFMpegDecoder::videoDecodeLoop() {
  AVFormatContext *fmt_ctx = nullptr;
  if (avformat_open_input(&fmt_ctx, m_path.toUtf8().constData(), nullptr, nullptr) < 0) {
    qWarning() << "Could not open input:" << m_path;
    return;
  }
  if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
    qWarning() << "Could not find stream info";
    avformat_close_input(&fmt_ctx);
    return;
  }

  int vid_idx = -1;
  for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
    AVCodecParameters *p = fmt_ctx->streams[i]->codecpar;
    if (p->codec_type == AVMEDIA_TYPE_VIDEO && vid_idx < 0)
      vid_idx = i;
  }
  if (vid_idx < 0) {
    avformat_close_input(&fmt_ctx);
    return;
  }

  AVCodec *vcodec = nullptr;
  AVCodec *iter = av_codec_next(nullptr);
  while (iter) {
    if (iter->id == fmt_ctx->streams[vid_idx]->codecpar->codec_id &&
        iter->decode != nullptr && iter->type == AVMEDIA_TYPE_VIDEO) {
      if (QString(iter->name).contains("rk", Qt::CaseInsensitive)) {
        iter = av_codec_next(iter);
        continue;
      }
      vcodec = iter;
      break;
    }
    iter = av_codec_next(iter);
  }
  if (!vcodec) {
    qWarning() << "Video decoder not found";
    avformat_close_input(&fmt_ctx);
    return;
  }
  AVCodecContext *vctx = avcodec_alloc_context3(vcodec);
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
  int vwidth = vctx->width;
  int vheight = vctx->height;
  AVRational vtime_base = fmt_ctx->streams[vid_idx]->time_base;
  int sws_src_pix_fmt = -1;
  SwsContext *sws_ctx = nullptr;

  // 视频输出 buffer
  int rgb_stride = vwidth * 3;
  uint8_t *rgb_buf = nullptr;
  if (vwidth && vheight) {
    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, vwidth, vheight, 1);
    rgb_buf = (uint8_t *)av_malloc(numBytes);
  }

  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();

  // 视频同步
  using clock = std::chrono::steady_clock;
  clock::time_point playback_start_time;

  // 计算总时长（只在视频线程发一次）
  qint64 duration_ms = fmt_ctx->duration >= 0 ? fmt_ctx->duration / (AV_TIME_BASE / 1000) : 0;
  emit durationChanged(duration_ms);

  while (!m_stop) {
    // pause
    if (m_pause) {
      std::unique_lock<std::mutex> lk(m_mutex);
      m_cond.wait(lk, [&] { return m_stop || !m_pause; });
      if (m_stop)
        break;
      playback_start_time = clock::now();
    }
    // seek
    if (m_seeking) {
      int64_t ts = m_seekTarget * (AV_TIME_BASE / 1000);
      av_seek_frame(fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers(vctx);
      m_seeking = false;
      playback_start_time = clock::now();
    }
    if (av_read_frame(fmt_ctx, pkt) < 0)
      break;
    if (pkt->stream_index != vid_idx) {
      av_packet_unref(pkt);
      continue;
    }
    avcodec_send_packet(vctx, pkt);
    while (avcodec_receive_frame(vctx, frame) == 0) {
      int64_t pts = frame->best_effort_timestamp;
      if (pts == AV_NOPTS_VALUE)
        pts = frame->pts;
      if (pts == AV_NOPTS_VALUE)
        pts = 0;
      int64_t ms = pts * vtime_base.num * 1000LL / vtime_base.den;

      // ----------- 音画同步逻辑 begin -----------
      qint64 audioClock = m_audioClockMs.load();
      qint64 diff = ms - audioClock;
      if (audioClock > 0) {
        if (diff > 40) {
          // 视频帧比音频快，分步短等待，提升高帧率流畅度
          int waited = 0;
          const int max_wait = 20; // 最多等待一帧时长（约16-20ms）
          while (diff > 5 && waited < max_wait && !m_stop && !m_pause && !m_seeking) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            waited += 5;
            audioClock = m_audioClockMs.load();
            diff = ms - audioClock;
          }
          if (diff > 40) {
            // 仍然超前，跳过显示该帧
            continue;
          }
        } else if (diff < -100) {
          // 视频帧比音频慢太多，丢弃该帧
          continue;
        }
        // 否则正常显示
      }
      // ----------- 音画同步逻辑 end -------------

      // 检查并初始化/重建 sws_ctx
      if (!sws_ctx || sws_src_pix_fmt != frame->format) {
        if (sws_ctx)
          sws_freeContext(sws_ctx);
        sws_ctx = sws_getCachedContext(
            nullptr, vwidth, vheight, (AVPixelFormat)frame->format, vwidth,
            vheight, AV_PIX_FMT_RGB24, SWS_BILINEAR | SWS_ACCURATE_RND,
            nullptr, nullptr, nullptr);
        sws_src_pix_fmt = frame->format;
        if (!sws_ctx) {
          qWarning() << "Could not initialize sws context for format"
                     << frame->format;
          continue;
        }
      }

      // 转为 RGB24
      uint8_t *dst[1] = {rgb_buf};
      int dst_linesize[1] = {rgb_stride};
      sws_scale(sws_ctx, frame->data, frame->linesize, 0, vheight, dst, dst_linesize);

      QImage img(rgb_buf, vwidth, vheight, rgb_stride, QImage::Format_RGB888);

      if (img.isNull()) {
        qWarning() << "QImage isNull after construction, fallback to copy buffer";
        QByteArray tmp((const char *)rgb_buf, vheight * rgb_stride);
        QImage img2((const uchar *)tmp.constData(), vwidth, vheight, QImage::Format_RGB888);
        emit frameReady(img2.copy());
      } else {
        emit frameReady(img.copy());
      }

      emit positionChanged(ms);
    }
    av_packet_unref(pkt);
  }

  if (rgb_buf)
    av_free(rgb_buf);
  av_frame_free(&frame);
  av_packet_free(&pkt);
  if (sws_ctx)
    sws_freeContext(sws_ctx);
  if (vctx)
    avcodec_free_context(&vctx);
  avformat_close_input(&fmt_ctx);
}

// 拆分后的音频解码循环
void FFMpegDecoder::audioDecodeLoop() {
  AVFormatContext *fmt_ctx = nullptr;
  if (avformat_open_input(&fmt_ctx, m_path.toUtf8().constData(), nullptr, nullptr) < 0) {
    qWarning() << "Could not open input:" << m_path;
    return;
  }
  if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
    qWarning() << "Could not find stream info";
    avformat_close_input(&fmt_ctx);
    return;
  }

  int aid_idx = -1;
  for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
    AVCodecParameters *p = fmt_ctx->streams[i]->codecpar;
    if (p->codec_type == AVMEDIA_TYPE_AUDIO && aid_idx < 0)
      aid_idx = i;
  }
  if (aid_idx < 0) {
    avformat_close_input(&fmt_ctx);
    return;
  }

  AVCodec *acodec = nullptr;
  AVCodec *iter = av_codec_next(nullptr);
  while (iter) {
    if (iter->id == fmt_ctx->streams[aid_idx]->codecpar->codec_id &&
        iter->decode != nullptr && iter->type == AVMEDIA_TYPE_AUDIO) {
      if (QString(iter->name).contains("rk", Qt::CaseInsensitive)) {
        iter = av_codec_next(iter);
        continue;
      }
      acodec = iter;
      break;
    }
    iter = av_codec_next(iter);
  }
  if (!acodec) {
    qWarning() << "Audio decoder not found";
    avformat_close_input(&fmt_ctx);
    return;
  }
  AVCodecContext *actx = avcodec_alloc_context3(acodec);
  if (!actx) {
    qWarning() << "Could not allocate audio codec context";
    avformat_close_input(&fmt_ctx);
    return;
  }
  if (avcodec_parameters_to_context(actx, fmt_ctx->streams[aid_idx]->codecpar) < 0) {
    qWarning() << "Could not copy audio codec parameters";
    avcodec_free_context(&actx);
    avformat_close_input(&fmt_ctx);
    return;
  }
  if (avcodec_open2(actx, acodec, nullptr) < 0) {
    qWarning() << "Could not open audio codec";
    avcodec_free_context(&actx);
    avformat_close_input(&fmt_ctx);
    return;
  }
  AVRational atime_base = fmt_ctx->streams[aid_idx]->time_base;
  SwrContext *swr_ctx = nullptr;
  if (actx->channel_layout == 0)
    actx->channel_layout = av_get_default_channel_layout(actx->channels);
  swr_ctx = swr_alloc_set_opts(
      nullptr,
      av_get_default_channel_layout(OUT_CHANNELS),
      OUT_SAMPLE_FMT,
      OUT_SAMPLE_RATE,
      actx->channel_layout,
      actx->sample_fmt,
      actx->sample_rate,
      0, nullptr);
  if (!swr_ctx) {
    qWarning() << "Could not allocate swr context";
  } else {
    if (swr_init(swr_ctx) < 0) {
      qWarning() << "Could not initialize swr context";
      swr_free(&swr_ctx);
      swr_ctx = nullptr;
    }
  }

  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();

  using clock = std::chrono::steady_clock;
  clock::time_point audio_playback_start_time;
  int64_t first_audio_pts = AV_NOPTS_VALUE;
  bool first_audio_frame = true;

  while (!m_stop) {
    // pause
    if (m_pause) {
      std::unique_lock<std::mutex> lk(m_mutex);
      m_cond.wait(lk, [&] { return m_stop || !m_pause; });
      if (m_stop)
        break;
      audio_playback_start_time = clock::now();
      first_audio_pts = AV_NOPTS_VALUE;
      first_audio_frame = true;
    }
    // seek
    if (m_seeking) {
      int64_t ts = m_seekTarget * (AV_TIME_BASE / 1000);
      av_seek_frame(fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers(actx);
      m_seeking = false;
      audio_playback_start_time = clock::now();
      first_audio_pts = AV_NOPTS_VALUE;
      first_audio_frame = true;
    }
    if (av_read_frame(fmt_ctx, pkt) < 0)
      break;
    if (pkt->stream_index != aid_idx) {
      av_packet_unref(pkt);
      continue;
    }
    avcodec_send_packet(actx, pkt);
    while (avcodec_receive_frame(actx, frame) == 0) {
      if (frame->nb_samples == 0) {
        continue;
      }
      int64_t pts = frame->pts;
      if (pts == AV_NOPTS_VALUE)
        pts = frame->best_effort_timestamp;
      if (pts == AV_NOPTS_VALUE)
        pts = 0;
      int64_t ms = pts * atime_base.num * 1000LL / atime_base.den;
      if (ms < 0) {
        continue;
      }
      // 更新音频时钟
      m_audioClockMs.store(ms);

      if (first_audio_frame) {
        audio_playback_start_time = clock::now();
        first_audio_pts = ms;
        first_audio_frame = false;
      } else {
        int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                              clock::now() - audio_playback_start_time)
                              .count();
        int64_t diff = (ms - first_audio_pts) - elapsed;
        if (diff > 0 && diff < 30) {
          std::this_thread::sleep_for(std::chrono::milliseconds(diff));
        }
      }

      // 重采样
      int out_nb = av_rescale_rnd(
          swr_get_delay(swr_ctx, actx->sample_rate) + frame->nb_samples,
          OUT_SAMPLE_RATE, actx->sample_rate, AV_ROUND_UP);
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

      emit positionChanged(ms);

      av_freep(&out_buf[0]);
      av_freep(&out_buf);
    }
    av_packet_unref(pkt);
  }

  av_frame_free(&frame);
  av_packet_free(&pkt);
  if (swr_ctx)
    swr_free(&swr_ctx);
  if (actx)
    avcodec_free_context(&actx);
  avformat_close_input(&fmt_ctx);
}