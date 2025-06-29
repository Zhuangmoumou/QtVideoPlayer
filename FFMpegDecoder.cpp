#include "FFMpegDecoder.h"
#include "qdebug.h"
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

// 构造函数，初始化 FFMpegDecoder 对象
FFMpegDecoder::FFMpegDecoder(QObject *parent) : QObject(parent) {
  // 注册所有的 FFMpeg 组件
  av_register_all();
}

FFMpegDecoder::~FFMpegDecoder() { stop(); }

void FFMpegDecoder::start(const QString &path) {
  // 停止解码器
  stop();
  // 设置解码器路径
  m_path = path;
  // 设置停止标志为 false
  m_stop = false;
  // 设置暂停标志为 false
  m_pause = false;
  // 设置 seek 标志为 false
  m_seeking = false;
  // 设置视频seek处理标志为false
  m_videoSeekHandled = false;
  // 设置音频seek处理标志为false
  m_audioSeekHandled = false;
  // 设置 eof 标志为 false
  m_eof = false;
  // 创建视频解码线程
  m_videoThread = std::thread(&FFMpegDecoder::videoDecodeLoop, this);
  // 创建音频解码线程
  m_audioThread = std::thread(&FFMpegDecoder::audioDecodeLoop, this);
}

void FFMpegDecoder::stop() {
  m_stop = true;
  m_eof = false;
  m_cond.notify_all();
  if (m_videoThread.joinable())
    m_videoThread.join();
  if (m_audioThread.joinable())
    m_audioThread.join();
}

void FFMpegDecoder::seek(qint64 ms) {
  m_seekTarget = ms;
  m_seeking = true;
  m_videoSeekHandled = false;
  m_audioSeekHandled = false;
  m_eof = false;
  m_cond.notify_all();
}

void FFMpegDecoder::togglePause() {
  m_pause = !m_pause;
  if (!m_pause)
    m_cond.notify_all();
}

bool FFMpegDecoder::isPaused() const { return m_pause; }

void FFMpegDecoder::videoDecodeLoop() {
  // 打开输入文件
  AVFormatContext *fmt_ctx = nullptr;
  AVDictionary *opts = nullptr;
  av_dict_set(&opts, "probe_size", "1048576", 0);
  av_dict_set(&opts, "analyzeduration", "1000000", 0);
  if (avformat_open_input(&fmt_ctx, m_path.toUtf8().constData(), nullptr,
                          &opts) < 0) {
    qWarning() << "Failed to open input file:" << m_path;
    av_dict_free(&opts);
    return;
  }
  av_dict_free(&opts);
  // 获取流信息
  if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
    qWarning() << "Failed to get stream info";
    avformat_close_input(&fmt_ctx);
    return;
  }
  // 获取视频流索引
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
  // 获取视频解码器
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
  // 分配视频解码器上下文
  AVCodecContext *vctx = avcodec_alloc_context3(vcodec);
  if (!vctx) {
    qWarning() << "Failed to allocate video decoder context";
    avformat_close_input(&fmt_ctx);
    return;
  }
  // 复制视频解码器参数
  if (avcodec_parameters_to_context(vctx, fmt_ctx->streams[vid_idx]->codecpar) <
      0) {
    qWarning() << "Failed to copy video decoder parameters";
    avcodec_free_context(&vctx);
    avformat_close_input(&fmt_ctx);
    return;
  }
  // 打开视频解码器
  if (avcodec_open2(vctx, vcodec, nullptr) < 0) {
    qWarning() << "Failed to open video decoder";
    avcodec_free_context(&vctx);
    avformat_close_input(&fmt_ctx);
    return;
  }
  // 获取视频宽高和时基
  int vwidth = vctx->width;
  int vheight = vctx->height;
  AVRational vtime_base = fmt_ctx->streams[vid_idx]->time_base;
  // 初始化 SwsContext 和 RGB 缓冲区
  int sws_src_pix_fmt = -1;
  SwsContext *sws_ctx = nullptr;
  int rgb_stride = vwidth * 3;
  uint8_t *rgb_buf = nullptr;
  int rgb_buf_size = 0;
  if (vwidth && vheight) {
    rgb_buf_size =
        av_image_get_buffer_size(AV_PIX_FMT_RGB24, vwidth, vheight, 1);
    rgb_buf = (uint8_t *)av_malloc(rgb_buf_size);
  }
  // 分配 AVPacket 和 AVFrame
  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  // 获取视频播放开始时间
  using clock = std::chrono::steady_clock;
  clock::time_point playback_start_time;
  // 获取视频总时长
  qint64 duration_ms =
      fmt_ctx->duration >= 0 ? fmt_ctx->duration / (AV_TIME_BASE / 1000) : 0;
  emit durationChanged(duration_ms);
  // 循环解码视频帧
  while (!m_stop) {
    // 暂停时等待
    if (m_pause) {
      std::unique_lock<std::mutex> lk(m_mutex);
      m_cond.wait(lk, [&] { return m_stop || !m_pause || m_seeking; });
      if (m_stop)
        break;
      playback_start_time = clock::now();
    }
    // 跳转时等待
    if (m_seeking) {
      int64_t ts = m_seekTarget * (AV_TIME_BASE / 1000);
      av_seek_frame(fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers(vctx);
      playback_start_time = clock::now();
      av_packet_unref(pkt);
      av_frame_unref(frame);
      {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_videoSeekHandled = true;
        if (m_audioSeekHandled) {
          m_seeking = false;
        }
      }
      continue;
    }
    // 读取视频帧
    if (av_read_frame(fmt_ctx, pkt) < 0) {
      m_eof = true;
      std::unique_lock<std::mutex> lk(m_mutex);
      m_cond.wait(lk, [&] { return m_stop || m_seeking || m_eof == false; });
      if (m_stop)
        break;
      if (m_seeking) {
        m_eof = false;
        continue;
      }
      continue;
    }
    // 判断是否为视频流
    if (pkt->stream_index != vid_idx) {
      av_packet_unref(pkt);
      continue;
    }
    // 跳转时等待
    if (m_seeking) {
      av_packet_unref(pkt);
      continue;
    }
    // 发送视频帧到解码器
    avcodec_send_packet(vctx, pkt);
    // 接收解码后的视频帧
    while (avcodec_receive_frame(vctx, frame) == 0) {
      // 跳转时等待
      if (m_seeking)
        break;
      // 获取视频帧的 PTS
      int64_t pts = frame->best_effort_timestamp;
      if (pts == AV_NOPTS_VALUE)
        pts = frame->pts;
      if (pts == AV_NOPTS_VALUE)
        pts = 0;
      // 转换 PTS 为毫秒
      int64_t ms = pts * vtime_base.num * 1000LL / vtime_base.den;
      // 获取音频时钟
      qint64 audioClock = m_audioClockMs.load();
      // 计算视频和音频的差值
      qint64 diff = ms - audioClock;
      // 如果音频时钟大于 0，则判断视频和音频的差值
      if (audioClock > 0) {
        // 如果视频帧比音频帧快，则等待
        if (diff > 80) {
          int waited = 0;
          const int max_wait = 40;
          while (diff > 10 && waited < max_wait && !m_stop && !m_pause &&
                 !m_seeking) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            waited += 5;
            audioClock = m_audioClockMs.load();
            diff = ms - audioClock;
          }
          // 如果视频帧比音频帧快超过 80ms，则丢弃该视频帧
          if (diff > 80) {
            qWarning() << "Drop video frame: video ahead of audio (diff ="
                       << diff << "ms, video ms =" << ms
                       << ", audio ms =" << audioClock << ")";
            continue;
          }
        // 如果视频帧比音频帧慢超过 300ms，则丢弃该视频帧
        } else if (diff < -300) {
          qWarning() << "Drop video frame: video lags audio too much (diff ="
                     << diff << "ms, video ms =" << ms
                     << ", audio ms =" << audioClock << ")";
          continue;
        }
      }
      // 初始化 SwsContext
      if (!sws_ctx || sws_src_pix_fmt != frame->format) {
        if (sws_ctx)
          sws_freeContext(sws_ctx);
        sws_ctx = sws_getCachedContext(
            nullptr, vwidth, vheight, (AVPixelFormat)frame->format, vwidth,
            vheight, AV_PIX_FMT_RGB24, SWS_FAST_BILINEAR, nullptr, nullptr,
            nullptr);
        sws_src_pix_fmt = frame->format;
        if (!sws_ctx) {
          qWarning() << "Failed to initialize sws context, format:"
                     << frame->format;
          continue;
        }
      }
      // 如果视频帧的宽高发生变化，则重新分配 RGB 缓冲区
      if (frame->width != vwidth || frame->height != vheight) {
        vwidth = frame->width;
        vheight = frame->height;
        rgb_stride = vwidth * 3;
        int new_buf_size =
            av_image_get_buffer_size(AV_PIX_FMT_RGB24, vwidth, vheight, 1);
        if (rgb_buf)
          av_free(rgb_buf);
        rgb_buf = (uint8_t *)av_malloc(new_buf_size);
        rgb_buf_size = new_buf_size;
      }
      // 转换视频帧格式为 RGB24
      uint8_t *dst[1] = {rgb_buf};
      int dst_linesize[1] = {rgb_stride};
      sws_scale(sws_ctx, frame->data, frame->linesize, 0, vheight, dst,
                dst_linesize);
      // 创建 QImage
      QImage img(rgb_buf, vwidth, vheight, rgb_stride, QImage::Format_RGB888);
      if (img.isNull()) {
        qWarning()
            << "QImage isNull after construction, fallback to buffer copy";
        QByteArray tmp((const char *)rgb_buf, vheight * rgb_stride);
        QImage img2((const uchar *)tmp.constData(), vwidth, vheight,
                    QImage::Format_RGB888);
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

void FFMpegDecoder::audioDecodeLoop() {
  // 创建 AVFormatContext 对象，用于打开输入文件
  AVFormatContext *fmt_ctx = nullptr;
  // 创建 AVDictionary 对象，用于设置打开输入文件的参数
  AVDictionary *opts = nullptr;
  // 设置 probe_size 参数，表示探测文件的大小
  av_dict_set(&opts, "probe_size", "1048576", 0);
  // 设置 analyzeduration 参数，表示分析文件的时间长度
  av_dict_set(&opts, "analyzeduration", "1000000", 0);
  // 打开输入文件
  if (avformat_open_input(&fmt_ctx, m_path.toUtf8().constData(), nullptr,
                          &opts) < 0) {
    qWarning() << "Failed to open input file:" << m_path;
    av_dict_free(&opts);
    return;
  }
  av_dict_free(&opts);
  // 获取输入文件的流信息
  if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
    qWarning() << "Failed to get stream info";
    avformat_close_input(&fmt_ctx);
    return;
  }
  // 获取音频流的索引
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
  // 获取音频解码器
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
  // 分配音频解码器上下文
  AVCodecContext *actx = avcodec_alloc_context3(acodec);
  if (!actx) {
    qWarning() << "Failed to allocate audio decoder context";
    avformat_close_input(&fmt_ctx);
    return;
  }
  // 复制音频解码器参数
  if (avcodec_parameters_to_context(actx, fmt_ctx->streams[aid_idx]->codecpar) <
      0) {
    qWarning() << "Failed to copy audio decoder parameters";
    avcodec_free_context(&actx);
    avformat_close_input(&fmt_ctx);
    return;
  }
  // 打开音频解码器
  if (avcodec_open2(actx, acodec, nullptr) < 0) {
    qWarning() << "Failed to open audio decoder";
    avcodec_free_context(&actx);
    avformat_close_input(&fmt_ctx);
    return;
  }
  // 获取音频流的时基
  AVRational atime_base = fmt_ctx->streams[aid_idx]->time_base;
  // 创建音频重采样上下文
  SwrContext *swr_ctx = nullptr;
  if (actx->channel_layout == 0)
    actx->channel_layout = av_get_default_channel_layout(actx->channels);
  swr_ctx =
      swr_alloc_set_opts(nullptr, av_get_default_channel_layout(OUT_CHANNELS),
                         OUT_SAMPLE_FMT, OUT_SAMPLE_RATE, actx->channel_layout,
                         actx->sample_fmt, actx->sample_rate, 0, nullptr);
  if (!swr_ctx || swr_init(swr_ctx) < 0) {
    qWarning() << "Failed to initialize audio resample context";
    swr_free(&swr_ctx);
    avformat_close_input(&fmt_ctx);
    return;
  }
  // 分配 AVPacket 和 AVFrame 对象
  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  // 分配输出缓冲区
  uint8_t **out_buf = nullptr;
  int out_buf_samples = 0;
  // 创建时钟对象
  using clock = std::chrono::steady_clock;
  // 记录音频播放开始时间
  clock::time_point audio_playback_start_time;
  // 记录第一个音频帧的pts
  int64_t first_audio_pts = AV_NOPTS_VALUE;
  // 记录是否是第一个音频帧
  bool first_audio_frame = true;
  // 循环解码音频帧
  while (!m_stop) {
    // 如果暂停，则等待
    if (m_pause) {
      std::unique_lock<std::mutex> lk(m_mutex);
      m_cond.wait(lk, [&] { return m_stop || !m_pause || m_seeking; });
      if (m_stop)
        break;
      audio_playback_start_time = clock::now();
      first_audio_pts = AV_NOPTS_VALUE;
      first_audio_frame = true;
    }
    // 如果正在 seek，则 seek 到指定位置
    if (m_seeking) {
      int64_t ts = m_seekTarget * (AV_TIME_BASE / 1000);
      av_seek_frame(fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers(actx);
      audio_playback_start_time = clock::now();
      first_audio_pts = AV_NOPTS_VALUE;
      first_audio_frame = true;
      av_packet_unref(pkt);
      av_frame_unref(frame);
      {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_audioSeekHandled = true;
        if (m_videoSeekHandled) {
          m_seeking = false;
        }
      }
      continue;
    }
    // 播放结束，等待后续操作
    if (av_read_frame(fmt_ctx, pkt) < 0) {
      m_eof = true;
      std::unique_lock<std::mutex> lk(m_mutex);
      m_cond.wait(lk, [&] { return m_stop || m_seeking || m_eof == false; });
      if (m_stop)
        break;
      if (m_seeking) {
        m_eof = false;
        continue;
      }
      continue;
    }
    // 如果不是音频流，则释放 AVPacket 对象并继续
    if (pkt->stream_index != aid_idx) {
      av_packet_unref(pkt);
      continue;
    }
    // 如果在 seeking，则释放 AVPacket 对象并继续
    if (m_seeking) {
      av_packet_unref(pkt);
      continue;
    }
    // 发送 AVPacket 到解码器
    avcodec_send_packet(actx, pkt);
    // 接收解码后的音频帧
    while (avcodec_receive_frame(actx, frame) == 0) {
      if (m_seeking)
        break;
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
      m_audioClockMs.store(ms);
      static double audio_drift_threshold = 0.005;
      static double audio_diff_avg_coef = 0.98;
      static double audio_diff_threshold = 0.03;
      static double audio_diff_avg = 0.0;
      if (first_audio_frame) {
        audio_playback_start_time = clock::now();
        first_audio_pts = ms;
        first_audio_frame = false;
      } else {
        int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                              clock::now() - audio_playback_start_time)
                              .count();
        double diff = (ms - first_audio_pts) - elapsed;
        if (std::abs(diff) < 500) {
          audio_diff_avg = audio_diff_avg * audio_diff_avg_coef +
                           diff * (1.0 - audio_diff_avg_coef);
        }
        double sync_threshold = std::max(
            5.0, std::min(audio_diff_threshold * elapsed * 0.001, 30.0));
        if (std::abs(diff) > sync_threshold) {
          if (diff > 0) {
            double delay = std::min(diff * 0.85, 40.0);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(delay)));
          } else {
            if (diff < -200) {
              continue;
            }
          }
        }
        if (std::abs(audio_diff_avg) > audio_drift_threshold) {
          auto adjustment =
              std::chrono::milliseconds(static_cast<int>(audio_diff_avg * 0.6));
          audio_playback_start_time += adjustment;
          audio_diff_avg *= 0.3;
        }
      }
      int out_nb = av_rescale_rnd(
          swr_get_delay(swr_ctx, actx->sample_rate) + frame->nb_samples,
          OUT_SAMPLE_RATE, actx->sample_rate, AV_ROUND_UP);
      if (out_nb > out_buf_samples) {
        if (out_buf) {
          av_freep(&out_buf[0]);
          av_freep(&out_buf);
        }
        av_samples_alloc_array_and_samples(&out_buf, nullptr, OUT_CHANNELS,
                                           out_nb, OUT_SAMPLE_FMT, 0);
        out_buf_samples = out_nb;
      }
      int converted =
          swr_convert(swr_ctx, out_buf, out_nb, (const uint8_t **)frame->data,
                      frame->nb_samples);
      int data_size = av_samples_get_buffer_size(nullptr, OUT_CHANNELS,
                                                 converted, OUT_SAMPLE_FMT, 1);
      QByteArray pcm((const char *)out_buf[0], data_size);
      emit audioReady(pcm);
      emit positionChanged(ms);
    }
    av_packet_unref(pkt);
  }
  // 清理资源
  if (out_buf) {
    av_freep(&out_buf[0]);
    av_freep(&out_buf);
  }
  av_frame_free(&frame);
  av_packet_free(&pkt);
  if (swr_ctx)
    swr_free(&swr_ctx);
  if (actx)
    avcodec_free_context(&actx);
  avformat_close_input(&fmt_ctx);
}