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

bool FFMpegDecoder::isPaused() const {
  return m_pause;
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
  int sws_src_pix_fmt = -1; // 新增，记录当前sws输入格式
  if (vid_idx >= 0) {
    // 强制使用软件解码器，排除 rkmpp
    AVCodec *vcodec = nullptr;
    AVCodec *iter = av_codec_next(nullptr);
    while (iter) {
      if (iter->id == fmt_ctx->streams[vid_idx]->codecpar->codec_id &&
          iter->decode != nullptr && iter->type == AVMEDIA_TYPE_VIDEO) {
        if (QString(iter->name).contains("rk", Qt::CaseInsensitive)) {
          iter = av_codec_next(iter);
          continue; // 跳过 rkmpp
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
    vctx = avcodec_alloc_context3(vcodec);
    if (!vctx) {
      qWarning() << "Could not allocate video codec context";
      avformat_close_input(&fmt_ctx);
      return;
    }
    if (avcodec_parameters_to_context(
            vctx, fmt_ctx->streams[vid_idx]->codecpar) < 0) {
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
    // sws_ctx 初始化延后到收到第一帧
  }
  // 音频解码器
  AVCodecContext *actx = nullptr;
  SwrContext *swr_ctx = nullptr;
  AVRational atime_base{0, 1};
  if (aid_idx >= 0) {
    // 强制使用软件解码器，排除 rkmpp
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
      if (vctx)
        avcodec_free_context(&vctx);
      avformat_close_input(&fmt_ctx);
      return;
    }
    actx = avcodec_alloc_context3(acodec);
    if (!actx) {
      qWarning() << "Could not allocate audio codec context";
      if (vctx)
        avcodec_free_context(&vctx);
      avformat_close_input(&fmt_ctx);
      return;
    }
    if (avcodec_parameters_to_context(
            actx, fmt_ctx->streams[aid_idx]->codecpar) < 0) {
      qWarning() << "Could not copy audio codec parameters";
      if (vctx)
        avcodec_free_context(&vctx);
      avcodec_free_context(&actx);
      avformat_close_input(&fmt_ctx);
      return;
    }
    if (avcodec_open2(actx, acodec, nullptr) < 0) {
      qWarning() << "Could not open audio codec";
      if (vctx)
        avcodec_free_context(&vctx);
      avcodec_free_context(&actx);
      avformat_close_input(&fmt_ctx);
      return;
    }
    atime_base = fmt_ctx->streams[aid_idx]->time_base;
    // swr 用于重采样到 S16/44100/2
    if (actx->channel_layout == 0)
      actx->channel_layout = av_get_default_channel_layout(actx->channels);
    swr_ctx = swr_alloc_set_opts(
        nullptr,
        av_get_default_channel_layout(OUT_CHANNELS), // 输出声道布局
        OUT_SAMPLE_FMT,                              // 输出采样格式
        OUT_SAMPLE_RATE,                             // 输出采样率
        actx->channel_layout,                        // 输入声道布局
        actx->sample_fmt,                            // 输入采样格式
        actx->sample_rate,                           // 输入采样率
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

  // 新增：用于视频同步的时间基准
  using clock = std::chrono::steady_clock;
  clock::time_point playback_start_time;
  int64_t first_video_pts = AV_NOPTS_VALUE;
  bool first_video_frame = true;

  // 新增：用于音频同步的时间基准
  clock::time_point audio_playback_start_time;
  int64_t first_audio_pts = AV_NOPTS_VALUE;
  bool first_audio_frame = true;
  int64_t last_audio_ms = -1; // 新增：记录上一个音频帧的ms

  // 主循环
  while (!m_stop) {
    // pause
    if (m_pause) {
      std::unique_lock<std::mutex> lk(m_mutex);
      m_cond.wait(lk, [&] { return m_stop || !m_pause; });
      if (m_stop)
        break;
      // 恢复播放时重置同步基准
      if (vid_idx >= 0) {
        playback_start_time = clock::now();
        first_video_pts = AV_NOPTS_VALUE;
        first_video_frame = true;
      }
      if (aid_idx >= 0) {
        audio_playback_start_time = clock::now();
        first_audio_pts = AV_NOPTS_VALUE;
        first_audio_frame = true;
        last_audio_ms = -1;
      }
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
      // seek 后重置同步基准
      if (vid_idx >= 0) {
        playback_start_time = clock::now();
        first_video_pts = AV_NOPTS_VALUE;
        first_video_frame = true;
      }
      if (aid_idx >= 0) {
        audio_playback_start_time = clock::now();
        first_audio_pts = AV_NOPTS_VALUE;
        first_audio_frame = true;
        last_audio_ms = -1;
      }
    }
    if (av_read_frame(fmt_ctx, pkt) < 0)
      break;

    // 视频包
    if (pkt->stream_index == vid_idx) {
      if (!vctx) {
        av_packet_unref(pkt);
        continue;
      }
      avcodec_send_packet(vctx, pkt);
      while (avcodec_receive_frame(vctx, frame) == 0) {
        // 新增：视频帧同步控制
        int64_t pts = frame->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE)
          pts = frame->pts;
        if (pts == AV_NOPTS_VALUE)
          pts = 0;
        int64_t ms = pts * vtime_base.num * 1000LL / vtime_base.den;

        if (first_video_frame) {
          playback_start_time = clock::now();
          first_video_pts = ms;
          first_video_frame = false;
        } else {
          int64_t elapsed =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  clock::now() - playback_start_time)
                  .count();
          int64_t wait_ms = (ms - first_video_pts) - elapsed;
          if (wait_ms > 0 && wait_ms < 2000) { // 防止长时间阻塞
            std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
          }
        }

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
        sws_scale(sws_ctx, frame->data, frame->linesize, 0, vheight, dst,
                  dst_linesize);

        QImage img(rgb_buf, vwidth, vheight, rgb_stride, QImage::Format_RGB888);

        if (img.isNull()) {
          qWarning()
              << "QImage isNull after construction, fallback to copy buffer";
          QByteArray tmp((const char *)rgb_buf, vheight * rgb_stride);
          QImage img2((const uchar *)tmp.constData(), vwidth, vheight,
                      QImage::Format_RGB888);
          emit frameReady(img2.copy());
        } else {
          emit frameReady(img.copy());
        }

        // 计算 pts 并发送
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
        if (frame->nb_samples == 0) {
          continue;
        }
        int64_t pts = frame->pts;
        if (pts == AV_NOPTS_VALUE)
          pts = frame->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE)
          pts = 0;
        int64_t ms = pts * atime_base.num * 1000LL / atime_base.den;

        // 跳过负时间戳的音频帧，避免首帧锯齿
        if (ms < 0) {
          continue;
        }

        // 修正：仅音频流时也能正确同步
        if (vid_idx < 0) {
          // 只有音频流时，独立音频同步
          if (first_audio_frame) {
            audio_playback_start_time = clock::now();
            first_audio_pts = ms;
            first_audio_frame = false;
            last_audio_ms = ms;
          } else {
            int64_t elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    clock::now() - audio_playback_start_time)
                    .count();
            int64_t diff = (ms - first_audio_pts) - elapsed;
            // 优化：只在 0 < diff < 30ms 时 sleep，避免长时间阻塞
            if (diff > 0 && diff < 30) {
              std::this_thread::sleep_for(std::chrono::milliseconds(diff));
            }
            last_audio_ms = ms;
          }
        } else {
          // 有视频流时，音频同步策略不变
          if (first_audio_frame) {
            audio_playback_start_time = clock::now();
            first_audio_pts = ms;
            first_audio_frame = false;
            last_audio_ms = ms;
          } else {
            int64_t elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    clock::now() - audio_playback_start_time)
                    .count();
            int64_t diff = (ms - first_audio_pts) - elapsed;
            // 优化：只在 0 < diff < 30ms 时 sleep，避免长时间阻塞
            if (diff > 0 && diff < 30) {
              std::this_thread::sleep_for(std::chrono::milliseconds(diff));
            }
            last_audio_ms = ms;
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