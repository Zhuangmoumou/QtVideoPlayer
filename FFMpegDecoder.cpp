#include "FFMpegDecoder.h"
#include "qdebug.h"
#include <QtDebug>
#include <chrono>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace {
// 智能指针管理 AVFrame
template<typename T, void (*FreeFunc)(T**)>
struct FFmpegDeleter {
    void operator()(T* ptr) const {
        if (ptr) {
            FreeFunc(&ptr);
        }
    }
};

using AVFramePtr = std::unique_ptr<AVFrame, FFmpegDeleter<AVFrame, av_frame_free>>;
using AVPacketPtr = std::unique_ptr<AVPacket, FFmpegDeleter<AVPacket, av_packet_free>>;
using AVCodecContextPtr = std::unique_ptr<AVCodecContext, FFmpegDeleter<AVCodecContext, avcodec_free_context>>;
using AVFormatContextPtr = std::unique_ptr<AVFormatContext, FFmpegDeleter<AVFormatContext, avformat_close_input>>;

AVFramePtr make_avframe() { return AVFramePtr(av_frame_alloc()); }
AVPacketPtr make_avpacket() { return AVPacketPtr(av_packet_alloc()); }
AVCodecContextPtr make_avcodec_ctx(AVCodec *codec) { return AVCodecContextPtr(avcodec_alloc_context3(codec)); }

// 查找解码器
AVCodec *find_decoder(AVCodecID id, AVMediaType type) {
  AVCodec *iter = av_codec_next(nullptr);
  while (iter) {
    if (iter->id == id && iter->decode != nullptr && iter->type == type) {
      if (QString(iter->name).contains("rk", Qt::CaseInsensitive)) {
        iter = av_codec_next(iter);
        continue;
      }
      return iter;
    }
    iter = av_codec_next(iter);
  }
  return nullptr;
}
} // namespace

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
  AVFormatContext *raw_fmt_ctx = nullptr;
  AVDictionary *opts = nullptr;
  av_dict_set(&opts, "probe_size", "1048576", 0);
  av_dict_set(&opts, "analyzeduration", "1000000", 0);
  if (avformat_open_input(&raw_fmt_ctx, m_path.toUtf8().constData(), nullptr,
                          &opts) < 0) {
    qWarning() << "Failed to open input file:" << m_path;
    av_dict_free(&opts);
    return;
  }
  av_dict_free(&opts);
  AVFormatContextPtr fmt_ctx(raw_fmt_ctx);
  // 获取流信息
  if (avformat_find_stream_info(fmt_ctx.get(), nullptr) < 0) {
    qWarning() << "Failed to get stream info";
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
    return;
  }
  // 获取视频解码器
  AVCodec *vcodec = find_decoder(fmt_ctx->streams[vid_idx]->codecpar->codec_id,
                                 AVMEDIA_TYPE_VIDEO);
  if (!vcodec) {
    qWarning() << "Video decoder not found";
    return;
  }
  // 分配视频解码器上下文
  AVCodecContextPtr vctx = make_avcodec_ctx(vcodec);
  if (!vctx) {
    qWarning() << "Failed to allocate video decoder context";
    return;
  }
  // 复制视频解码器参数
  if (avcodec_parameters_to_context(vctx.get(), fmt_ctx->streams[vid_idx]->codecpar) <
      0) {
    qWarning() << "Failed to copy video decoder parameters";
    return;
  }
  // 打开视频解码器
  if (avcodec_open2(vctx.get(), vcodec, nullptr) < 0) {
    qWarning() << "Failed to open video decoder";
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
  AVPacketPtr pkt = make_avpacket();
  AVFramePtr frame = make_avframe();
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
      av_seek_frame(fmt_ctx.get(), -1, ts, AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers(vctx.get());
      playback_start_time = clock::now();
      av_packet_unref(pkt.get());
      av_frame_unref(frame.get());
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
    if (av_read_frame(fmt_ctx.get(), pkt.get()) < 0) {
      m_eof = true;
      std::unique_lock<std::mutex> lk(m_mutex);
      m_cond.wait_for(lk, std::chrono::milliseconds(30), [&] { return m_stop || m_seeking || m_eof == false; });
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
      av_packet_unref(pkt.get());
      continue;
    }
    // 跳转时等待
    if (m_seeking) {
      av_packet_unref(pkt.get());
      continue;
    }
    // 发送视频帧到解码器
    avcodec_send_packet(vctx.get(), pkt.get());
    // 接收解码后的视频帧
    while (avcodec_receive_frame(vctx.get(), frame.get()) == 0) {
      if (m_seeking)
        break;
      int64_t pts = frame->best_effort_timestamp;
      if (pts == AV_NOPTS_VALUE)
        pts = frame->pts;
      if (pts == AV_NOPTS_VALUE)
        pts = 0;
      int64_t ms = pts * vtime_base.num * 1000LL / vtime_base.den;
      qint64 audioClock = m_audioClockMs.load();
      qint64 diff = ms - audioClock;
      // 计算自适应丢帧/等待阈值（如帧间隔的2倍，最小10ms最大80ms）
      int frame_interval = 40; // 默认25fps
      if (vctx->framerate.num && vctx->framerate.den) {
        frame_interval = 1000 * vctx->framerate.den / vctx->framerate.num;
        frame_interval = std::max(10, std::min(frame_interval, 80));
      }
      int max_wait = frame_interval * 2;
      // 音画同步：视频快于音频时等待，慢于音频时丢帧
      if (audioClock > 0) {
        if (diff > frame_interval) {
          int waited = 0;
          while (diff > 5 && waited < max_wait && !m_stop && !m_pause && !m_seeking) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            waited += 2;
            audioClock = m_audioClockMs.load();
            diff = ms - audioClock;
          }
          if (diff > frame_interval) {
            qWarning() << "[Sync] Drop video frame: video ahead (diff =" << diff << "ms, video =" << ms << ", audio =" << audioClock << ")";
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
          }
        } else if (diff < -frame_interval * 6) { // 允许最大音画滞后
          qWarning() << "[Sync] Drop video frame: video lags (diff =" << diff << "ms, video =" << ms << ", audio =" << audioClock << ")";
          continue;
        }
      }
      // 初始化 SwsContext
      if (!sws_ctx || sws_src_pix_fmt != frame->format || frame->width != vwidth || frame->height != vheight) {
        if (sws_ctx)
          sws_freeContext(sws_ctx);
        vwidth = frame->width;
        vheight = frame->height;
        rgb_stride = vwidth * 3;
        int new_buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, vwidth, vheight, 1);
        if (new_buf_size != rgb_buf_size) {
          if (rgb_buf)
            av_free(rgb_buf);
          rgb_buf = (uint8_t *)av_malloc(new_buf_size);
          rgb_buf_size = new_buf_size;
        }
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
      // 转换视频帧格式为 RGB24
      uint8_t *dst[1] = {rgb_buf};
      int dst_linesize[1] = {rgb_stride};
      sws_scale(sws_ctx, frame->data, frame->linesize, 0, vheight, dst,
                dst_linesize);
      // 只在必要时复制 QImage
      QImage img(rgb_buf, vwidth, vheight, rgb_stride, QImage::Format_RGB888);
      if (img.isNull()) {
        qWarning() << "QImage isNull after construction, fallback to buffer copy";
        QByteArray tmp = QByteArray::fromRawData((const char *)rgb_buf, vheight * rgb_stride);
        QImage img2((const uchar *)tmp.constData(), vwidth, vheight, QImage::Format_RGB888);
        emit frameReady(img2.copy());
      } else {
        emit frameReady(img.copy());
      }
      emit positionChanged(ms);
    }
    av_packet_unref(pkt.get());
  }
  if (rgb_buf)
    av_free(rgb_buf);
  if (sws_ctx)
    sws_freeContext(sws_ctx);
}

void FFMpegDecoder::audioDecodeLoop() {
  AVFormatContext *raw_fmt_ctx = nullptr;
  AVDictionary *opts = nullptr;
  av_dict_set(&opts, "probe_size", "1048576", 0);
  av_dict_set(&opts, "analyzeduration", "1000000", 0);
  if (avformat_open_input(&raw_fmt_ctx, m_path.toUtf8().constData(), nullptr, &opts) < 0) {
    qWarning() << "Failed to open input file:" << m_path;
    av_dict_free(&opts);
    return;
  }
  av_dict_free(&opts);
  AVFormatContextPtr fmt_ctx(raw_fmt_ctx);
  if (avformat_find_stream_info(fmt_ctx.get(), nullptr) < 0) {
    qWarning() << "Failed to get stream info";
    return;
  }
  int aid_idx = -1;
  for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
    AVCodecParameters *p = fmt_ctx->streams[i]->codecpar;
    if (p->codec_type == AVMEDIA_TYPE_AUDIO && aid_idx < 0)
      aid_idx = i;
  }
  if (aid_idx < 0) {
    return;
  }
  AVCodec *acodec = find_decoder(fmt_ctx->streams[aid_idx]->codecpar->codec_id, AVMEDIA_TYPE_AUDIO);
  if (!acodec) {
    qWarning() << "Audio decoder not found";
    return;
  }
  AVCodecContextPtr actx = make_avcodec_ctx(acodec);
  if (!actx) {
    qWarning() << "Failed to allocate audio decoder context";
    return;
  }
  if (avcodec_parameters_to_context(actx.get(), fmt_ctx->streams[aid_idx]->codecpar) < 0) {
    qWarning() << "Failed to copy audio decoder parameters";
    return;
  }
  if (avcodec_open2(actx.get(), acodec, nullptr) < 0) {
    qWarning() << "Failed to open audio decoder";
    return;
  }
  AVRational atime_base = fmt_ctx->streams[aid_idx]->time_base;
  SwrContext *swr_ctx = nullptr;
  if (actx->channel_layout == 0)
    actx->channel_layout = av_get_default_channel_layout(actx->channels);
  swr_ctx = swr_alloc_set_opts(nullptr, av_get_default_channel_layout(OUT_CHANNELS), OUT_SAMPLE_FMT, OUT_SAMPLE_RATE, actx->channel_layout, actx->sample_fmt, actx->sample_rate, 0, nullptr);
  if (!swr_ctx || swr_init(swr_ctx) < 0) {
    qWarning() << "Failed to initialize audio resample context";
    swr_free(&swr_ctx);
    return;
  }
  AVPacketPtr pkt = make_avpacket();
  AVFramePtr frame = make_avframe();
  uint8_t **out_buf = nullptr;
  int out_buf_samples = 0;
  using clock = std::chrono::steady_clock;
  clock::time_point audio_playback_start_time;
  int64_t first_audio_pts = AV_NOPTS_VALUE;
  bool first_audio_frame = true;
  while (!m_stop) {
    if (m_pause) {
      std::unique_lock<std::mutex> lk(m_mutex);
      m_cond.wait(lk, [&] { return m_stop || !m_pause || m_seeking; });
      if (m_stop)
        break;
      audio_playback_start_time = clock::now();
      first_audio_pts = AV_NOPTS_VALUE;
      first_audio_frame = true;
    }
    if (m_seeking) {
      int64_t ts = m_seekTarget * (AV_TIME_BASE / 1000);
      av_seek_frame(fmt_ctx.get(), -1, ts, AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers(actx.get());
      audio_playback_start_time = clock::now();
      first_audio_pts = AV_NOPTS_VALUE;
      first_audio_frame = true;
      av_packet_unref(pkt.get());
      av_frame_unref(frame.get());
      {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_audioSeekHandled = true;
        if (m_videoSeekHandled) {
          m_seeking = false;
        }
      }
      continue;
    }
    if (av_read_frame(fmt_ctx.get(), pkt.get()) < 0) {
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
    if (pkt->stream_index != aid_idx) {
      av_packet_unref(pkt.get());
      continue;
    }
    if (m_seeking) {
      av_packet_unref(pkt.get());
      continue;
    }
    avcodec_send_packet(actx.get(), pkt.get());
    while (avcodec_receive_frame(actx.get(), frame.get()) == 0) {
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
        int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - audio_playback_start_time).count();
        double diff = (ms - first_audio_pts) - elapsed;
        if (std::abs(diff) < 500) {
          audio_diff_avg = audio_diff_avg * audio_diff_avg_coef + diff * (1.0 - audio_diff_avg_coef);
        }
        double sync_threshold = std::max(5.0, std::min(audio_diff_threshold * elapsed * 0.001, 30.0));
        if (std::abs(diff) > sync_threshold) {
          if (diff > 0) {
            double delay = std::min(diff * 0.85, 40.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(delay)));
          } else {
            if (diff < -200) {
              continue;
            }
          }
        }
        if (std::abs(audio_diff_avg) > audio_drift_threshold) {
          auto adjustment = std::chrono::milliseconds(static_cast<int>(audio_diff_avg * 0.6));
          audio_playback_start_time += adjustment;
          audio_diff_avg *= 0.3;
        }
      }
      int out_nb = av_rescale_rnd(swr_get_delay(swr_ctx, actx->sample_rate) + frame->nb_samples, OUT_SAMPLE_RATE, actx->sample_rate, AV_ROUND_UP);
      if (out_nb > out_buf_samples) {
        if (out_buf) {
          av_freep(&out_buf[0]);
          av_freep(&out_buf);
        }
        av_samples_alloc_array_and_samples(&out_buf, nullptr, OUT_CHANNELS, out_nb, OUT_SAMPLE_FMT, 0);
        out_buf_samples = out_nb;
      }
      int converted = swr_convert(swr_ctx, out_buf, out_nb, (const uint8_t **)frame->data, frame->nb_samples);
      int data_size = av_samples_get_buffer_size(nullptr, OUT_CHANNELS, converted, OUT_SAMPLE_FMT, 1);
      QByteArray pcm = QByteArray::fromRawData((const char *)out_buf[0], data_size);
      emit audioReady(pcm);
      emit positionChanged(ms);
    }
    av_packet_unref(pkt.get());
  }
  if (out_buf) {
    av_freep(&out_buf[0]);
    av_freep(&out_buf);
  }
  if (swr_ctx)
    swr_free(&swr_ctx);
}