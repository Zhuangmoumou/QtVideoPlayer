#include "FFMpegDecoder.h"
#include "qdebug.h"
#include <QSharedPointer>
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
template <typename T, void (*FreeFunc)(T **)> struct FFmpegDeleter {
  void operator()(T *ptr) const {
    if (ptr) {
      FreeFunc(&ptr);
    }
  }
};

using AVFramePtr =
    std::unique_ptr<AVFrame, FFmpegDeleter<AVFrame, av_frame_free>>;
using AVPacketPtr =
    std::unique_ptr<AVPacket, FFmpegDeleter<AVPacket, av_packet_free>>;
using AVCodecContextPtr =
    std::unique_ptr<AVCodecContext,
                    FFmpegDeleter<AVCodecContext, avcodec_free_context>>;
using AVFormatContextPtr =
    std::unique_ptr<AVFormatContext,
                    FFmpegDeleter<AVFormatContext, avformat_close_input>>;

AVFramePtr make_avframe() { return AVFramePtr(av_frame_alloc()); }
AVPacketPtr make_avpacket() { return AVPacketPtr(av_packet_alloc()); }
AVCodecContextPtr make_avcodec_ctx(AVCodec *codec) {
  return AVCodecContextPtr(avcodec_alloc_context3(codec));
}

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

  // 注册 QSharedPointer<QImage> 类型，以便在信号槽中使用
  qRegisterMetaType<QSharedPointer<QImage>>("QSharedPointer<QImage>");
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
  // 设置视频 seek 处理标志为 false
  m_videoSeekHandled = false;
  // 设置音频 seek 处理标志为 false
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

void FFMpegDecoder::setAudioTrack(int index) {
  std::lock_guard<std::mutex> lk(m_mutex);
  if (index < -1 || index >= static_cast<int>(m_audioStreamIndices.size()))
    return;
  if (m_audioTrackIndex != index) {
    m_audioTrackIndex = index;
    m_seeking = true;
    m_videoSeekHandled = false;
    m_audioSeekHandled = false;
    m_eof = false;
    m_cond.notify_all();
  }
}

int FFMpegDecoder::audioTrackCount() const {
  return static_cast<int>(m_audioStreamIndices.size());
}

int FFMpegDecoder::currentAudioTrack() const { return m_audioTrackIndex; }

QString FFMpegDecoder::audioTrackName(int idx) const {
  if (idx < 0 || idx >= static_cast<int>(m_audioStreamNames.size()))
    return QString();
  return m_audioStreamNames[idx];
}

void FFMpegDecoder::setVideoTrack(int index) {
  std::lock_guard<std::mutex> lk(m_mutex);
  // 允许 index == -1，表示空轨道
  if (index < -1 || index >= static_cast<int>(m_videoStreamIndices.size()))
    return;
  if (m_videoTrackIndex != index) {
    m_videoTrackIndex = index;
    if (index == -1) {
      m_seeking = true;
      m_videoSeekHandled = false;
      m_cond.notify_all();
      emit frameReady(QSharedPointer<QImage>());
    } else {
      m_seeking = true;
      m_videoSeekHandled = false;
      m_audioSeekHandled = false;
      m_eof = false;
      m_cond.notify_all();
    }
  }
}

int FFMpegDecoder::videoTrackCount() const {
  return static_cast<int>(m_videoStreamIndices.size());
}

int FFMpegDecoder::currentVideoTrack() const { return m_videoTrackIndex; }

QString FFMpegDecoder::videoTrackName(int idx) const {
  if (idx < 0 || idx >= static_cast<int>(m_videoStreamNames.size()))
    return QString();
  return m_videoStreamNames[idx];
}

void FFMpegDecoder::videoDecodeLoop() {
  while (!m_stop) {
    // 打开输入文件
    AVFormatContext *raw_fmt_ctx = nullptr;
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "probe_size", "1048576", 0);
    av_dict_set(&opts, "analyzeduration", "1000000", 0);
    if (avformat_open_input(&raw_fmt_ctx, m_path.toUtf8().constData(), nullptr,
                            &opts) < 0) {
      qWarning() << "Failed to open input file:" << m_path;
      emit errorOccurred(tr("无法打开文件: %1").arg(m_path));
      av_dict_free(&opts);
      return;
    }
    av_dict_free(&opts);
    AVFormatContextPtr fmt_ctx(raw_fmt_ctx);
    if (avformat_find_stream_info(fmt_ctx.get(), nullptr) < 0) {
      qWarning() << "Failed to get stream info";
      emit errorOccurred(tr("无法获取媒体流信息"));
      return;
    }
    // 获取所有视频流索引和名称
    m_videoStreamIndices.clear();
    m_videoStreamNames.clear();
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
      AVCodecParameters *p = fmt_ctx->streams[i]->codecpar;
      if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
        m_videoStreamIndices.push_back(i);
        QString name = QString("Track %1").arg(m_videoStreamIndices.size());
        if (fmt_ctx->streams[i]->metadata) {
          AVDictionaryEntry *lang = av_dict_get(fmt_ctx->streams[i]->metadata,
                                                "language", nullptr, 0);
          if (lang && lang->value)
            name += QString(" [%1]").arg(lang->value);
        }
        m_videoStreamNames.push_back(name);
      }
    }
    if (m_videoTrackIndex >= static_cast<int>(m_videoStreamIndices.size()))
      m_videoTrackIndex = m_videoStreamIndices.empty() ? -1 : 0;

    // 简化为主循环：统一处理空轨道和视频轨道
    qint64 duration_ms =
        fmt_ctx->duration >= 0 ? fmt_ctx->duration / (AV_TIME_BASE / 1000) : 0;
    emit durationChanged(duration_ms);

    // 资源初始化（移出循环）
    AVCodec *vcodec = nullptr;
    AVCodecContextPtr vctx;
    int vwidth = 0, vheight = 0;
    AVRational vtime_base = {0, 1};
    int sws_src_pix_fmt = -1;
    SwsContext *sws_ctx = nullptr;
    int rgb_stride = 0;
    uint8_t *rgb_buf = nullptr;
    int rgb_buf_size = 0;
    AVPacketPtr pkt = make_avpacket();
    AVFramePtr frame = make_avframe();
    using clock = std::chrono::steady_clock;
    clock::time_point playback_start_time = clock::now();

    while (!m_stop) {
      // 获取当前视频轨道索引
      int vid_idx = -1;
      {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_videoTrackIndex >= 0 &&
            m_videoTrackIndex < static_cast<int>(m_videoStreamIndices.size()))
          vid_idx = m_videoStreamIndices[m_videoTrackIndex];
      }

      // 处理空轨道
      if (vid_idx < 0) {
        // 清空画面
        emit frameReady(QSharedPointer<QImage>());

        // 暂停或等待状态变化
        if (m_pause) {
          std::unique_lock<std::mutex> lk(m_mutex);
          m_cond.wait(lk, [&] {
            return m_stop || !m_pause || m_seeking || m_videoTrackIndex != -1;
          });
          if (m_stop)
            break;
        }

        // 处理 seek
        if (m_seeking) {
          m_audioClockMs.store(m_seekTarget);
          std::lock_guard<std::mutex> lk(m_mutex);
          m_videoSeekHandled = true;
          if (m_audioSeekHandled)
            m_seeking = false;
          continue;
        }

        // 推进位置（基于音频时钟）
        emit positionChanged(m_audioClockMs.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        continue;
      }

      // 初始化/重置视频解码资源（如果轨道变化）
      if (!vctx || vid_idx !=
                       m_videoStreamIndices
                           [m_videoTrackIndex]) { // 假设轨道变化时重新初始化
        vcodec = find_decoder(fmt_ctx->streams[vid_idx]->codecpar->codec_id,
                              AVMEDIA_TYPE_VIDEO);
        if (!vcodec) {
          qWarning() << "Video decoder not found";
          emit errorOccurred(tr("未找到视频解码器"));
          break;
        }
        vctx = make_avcodec_ctx(vcodec);
        if (!vctx) {
          qWarning() << "Failed to allocate video decoder context";
          emit errorOccurred(tr("无法分配视频解码器上下文"));
          break;
        }
        if (avcodec_parameters_to_context(
                vctx.get(), fmt_ctx->streams[vid_idx]->codecpar) < 0) {
          qWarning() << "Failed to copy video decoder parameters";
          emit errorOccurred(tr("无法复制视频解码器参数"));
          break;
        }
        if (avcodec_open2(vctx.get(), vcodec, nullptr) < 0) {
          qWarning() << "Failed to open video decoder";
          emit errorOccurred(tr("无法打开视频解码器"));
          break;
        }
        vwidth = vctx->width;
        vheight = vctx->height;
        vtime_base = fmt_ctx->streams[vid_idx]->time_base;
        if (sws_ctx)
          sws_freeContext(sws_ctx);
        sws_ctx = nullptr;
        if (rgb_buf)
          av_free(rgb_buf);
        rgb_buf = nullptr;
        rgb_buf_size = 0;
        if (vwidth && vheight) {
          rgb_buf_size =
              av_image_get_buffer_size(AV_PIX_FMT_RGB24, vwidth, vheight, 1);
          rgb_buf = (uint8_t *)av_malloc(rgb_buf_size);
        }
      }

      // 暂停处理
      if (m_pause) {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_cond.wait(lk, [&] { return m_stop || !m_pause || m_seeking; });
        if (m_stop)
          break;
        playback_start_time = clock::now();
      }

      // 跳转处理
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
          if (m_audioSeekHandled)
            m_seeking = false;
        }
        continue;
      }

      // 读取视频帧
      if (av_read_frame(fmt_ctx.get(), pkt.get()) < 0) {
        m_eof = true;
        std::unique_lock<std::mutex> lk(m_mutex);
        m_cond.wait_for(lk, std::chrono::milliseconds(50),
                        [&] { return m_stop || m_seeking || m_eof == false; });
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

      // 发送视频帧到解码器
      avcodec_send_packet(vctx.get(), pkt.get());

      // 接收解码后的视频帧
      while (!m_stop && !m_seeking &&
             avcodec_receive_frame(vctx.get(), frame.get()) == 0) {
        double speed = m_playbackSpeed.load();

        int64_t pts = frame->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE)
          pts = frame->pts;
        if (pts == AV_NOPTS_VALUE)
          pts = 0;
        int64_t ms = pts * vtime_base.num * 1000LL / vtime_base.den;
        qint64 audioClock = m_audioClockMs.load();
        qint64 diff = ms - audioClock;

        bool hasAudio = (m_audioTrackIndex != -1);
        int frame_interval = 40;
        if (vctx->framerate.num && vctx->framerate.den) {
          frame_interval = 1000 * vctx->framerate.den / vctx->framerate.num;
          frame_interval = std::max(10, std::min(frame_interval, 80));
        }
        int max_wait = frame_interval * 2;

        if (hasAudio && audioClock > 0) {
          if (diff > frame_interval) {
            int waited = 0;
            if (diff > 20 && waited < max_wait && !m_stop && !m_pause &&
                !m_seeking) {
              int sleep_time = static_cast<int>(diff * 0.8 / speed);
              std::this_thread::sleep_for(
                  std::chrono::milliseconds(sleep_time));
              waited += sleep_time;
              audioClock = m_audioClockMs.load();
              diff = ms - audioClock;
            }
            while (diff > 5 && waited < max_wait && !m_stop && !m_pause &&
                   !m_seeking) {
              std::this_thread::sleep_for(std::chrono::milliseconds(5));
              waited += 5;
              audioClock = m_audioClockMs.load();
              diff = ms - audioClock;
            }
            if (m_stop || m_seeking || m_pause)
              break;
            if (diff > frame_interval)
              continue;
          } else if (diff < -frame_interval * 6) {
            continue;
          }
        }

        if (!hasAudio) {
          static qint64 last_video_pts = 0;
          static auto last_wall_clock = clock::now();
          static float last_video_speed = 1.0f;

          float speed = m_playbackSpeed.load(); // 加入倍速控制

          // 检测速度变化，如果速度变化超过阈值，重置视频同步参考点
          bool speed_changed = fabs(speed - last_video_speed) > 0.1f;
          if (speed_changed) {
            last_video_pts = 0; // 强制重置参考点
            last_video_speed = speed;
          }

          if (last_video_pts == 0 || ms < last_video_pts || speed_changed) {
            last_video_pts = ms;
            last_wall_clock = clock::now();
          } else {
            qint64 pts_diff = ms - last_video_pts;
            auto now = clock::now();
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_wall_clock)
                    .count();

            if (!m_stop && !m_seeking && !m_pause &&
                elapsed < pts_diff / speed) {
              std::this_thread::sleep_for(std::chrono::milliseconds(
                  static_cast<int>((pts_diff / speed) - elapsed)));
            }

            if (!m_stop && !m_seeking) {
              last_video_pts = ms;
              last_wall_clock = clock::now();
            }
          }
        }

        if (m_stop || m_seeking)
          break;

        // 初始化 SwsContext
        if (!sws_ctx || sws_src_pix_fmt != frame->format ||
            frame->width != vwidth || frame->height != vheight) {
          if (sws_ctx)
            sws_freeContext(sws_ctx);
          vwidth = frame->width;
          vheight = frame->height;
          rgb_stride = vwidth * 3;
          int new_buf_size =
              av_image_get_buffer_size(AV_PIX_FMT_RGB24, vwidth, vheight, 1);
          if (new_buf_size != rgb_buf_size) {
            if (rgb_buf)
              av_free(rgb_buf);
            rgb_buf = (uint8_t *)av_malloc(new_buf_size);
            rgb_buf_size = new_buf_size;
          }
          sws_ctx = sws_getCachedContext(
              nullptr, vwidth, vheight, (AVPixelFormat)frame->format, vwidth,
              vheight, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr,
              nullptr);
          sws_src_pix_fmt = frame->format;
          if (!sws_ctx)
            continue;
        }

        if (!rgb_buf) {
          rgb_buf_size =
              av_image_get_buffer_size(AV_PIX_FMT_RGB24, vwidth, vheight, 1);
          rgb_buf = (uint8_t *)av_malloc(rgb_buf_size);
          if (!rgb_buf)
            continue;
        }

        // 转换格式
        uint8_t *dst[1] = {rgb_buf};
        int dst_linesize[1] = {rgb_stride};
        sws_scale(sws_ctx, frame->data, frame->linesize, 0, vheight, dst,
                  dst_linesize);

        // 创建 QImage
        struct RGBBufferDeleter {
          void operator()(QImage *img) { delete img; }
        };
        QSharedPointer<QImage> imgPtr;
        if (rgb_buf) {
          QImage *rawImg = new QImage(
              rgb_buf, vwidth, vheight, rgb_stride, QImage::Format_RGB888,
              [](void *buf) { av_free(buf); }, rgb_buf);
          if (!rawImg->isNull()) {
            imgPtr = QSharedPointer<QImage>(rawImg, RGBBufferDeleter());
            rgb_buf = nullptr;
          } else {
            delete rawImg;
            QImage tempImg(rgb_buf, vwidth, vheight, rgb_stride,
                           QImage::Format_RGB888);
            imgPtr = QSharedPointer<QImage>(new QImage(tempImg.copy()));
            av_free(rgb_buf);
            rgb_buf = nullptr;
          }
        }
        emit frameReady(imgPtr);
        emit positionChanged(ms);
      }
      av_packet_unref(pkt.get());
    }

    // 清理资源
    if (rgb_buf)
      av_free(rgb_buf);
    if (sws_ctx)
      sws_freeContext(sws_ctx);
  }
}

void FFMpegDecoder::audioDecodeLoop() {
  // 打开输入文件和查找流信息
  AVFormatContext *raw_fmt_ctx = nullptr;
  AVDictionary *opts = nullptr;
  av_dict_set(&opts, "probe_size", "1048576", 0);
  av_dict_set(&opts, "analyzeduration", "1000000", 0);
  if (avformat_open_input(&raw_fmt_ctx, m_path.toUtf8().constData(), nullptr,
                          &opts) < 0) {
    qWarning() << "Failed to open input file:" << m_path;
    emit errorOccurred(tr("无法打开文件: %1").arg(m_path));
    av_dict_free(&opts);
    return;
  }
  av_dict_free(&opts);
  AVFormatContextPtr fmt_ctx(raw_fmt_ctx);
  if (avformat_find_stream_info(fmt_ctx.get(), nullptr) < 0) {
    qWarning() << "Failed to get stream info";
    emit errorOccurred(tr("无法获取媒体流信息"));
    return;
  }

  // 收集所有音频流
  {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_audioStreamIndices.clear();
    m_audioStreamNames.clear();
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
      if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        m_audioStreamIndices.push_back(i);
        QString name = QString("Track %1").arg(m_audioStreamIndices.size());
        AVDictionaryEntry *lang =
            av_dict_get(fmt_ctx->streams[i]->metadata, "language", nullptr, 0);
        if (lang && lang->value) {
          name += QString(" [%1]").arg(lang->value);
        }
        m_audioStreamNames.push_back(name);
      }
    }
    if (m_audioTrackIndex >= static_cast<int>(m_audioStreamIndices.size())) {
      m_audioTrackIndex = m_audioStreamIndices.empty() ? -1 : 0;
    }
  }

  // 资源和状态变量初始化
  AVCodecContextPtr actx = nullptr;
  SwrContext *swr_ctx = nullptr;
  AVPacketPtr pkt = make_avpacket();
  AVFramePtr frame = make_avframe();
  uint8_t **out_buf = nullptr;
  int out_buf_samples = 0;
  int last_audio_stream_id = -1; // 用于检测音轨切换
  AVRational atime_base = {0, 1};

  // --- 同步状态变量 ---
  using clock = std::chrono::steady_clock;
  clock::time_point audio_playback_start_time;
  int64_t first_audio_pts;
  bool first_audio_frame;
  double audio_diff_avg;
  // --- 同步状态变量结束 ---

  // 创建一个集中的函数来重置解码器和同步状态
  auto reset_decoder_and_sync_state = [&]() {
    // 清空解码器内部缓冲区
    if (actx) {
      avcodec_flush_buffers(actx.get());
    }
    // 清空重采样器内部缓冲区
    if (swr_ctx) {
      swr_convert(swr_ctx, nullptr, 0, nullptr, 0);
    }
    // 重置同步计时器和状态
    audio_playback_start_time = clock::now();
    first_audio_pts = AV_NOPTS_VALUE;
    first_audio_frame = true;
    audio_diff_avg = 0.0;
    m_audioClockMs.store(0);
    // 清理可能正在处理的 packet 和 frame
    av_packet_unref(pkt.get());
    av_frame_unref(frame.get());
  };

  reset_decoder_and_sync_state(); // 初始状态设置

  // 主解码循环
  while (!m_stop) {
    // 获取当前应播放的音轨索引
    int current_track_index = -1;
    int current_stream_id = -1;
    {
      std::lock_guard<std::mutex> lk(m_mutex);
      current_track_index = m_audioTrackIndex;
      if (current_track_index >= 0 &&
          current_track_index < static_cast<int>(m_audioStreamIndices.size())) {
        current_stream_id = m_audioStreamIndices[current_track_index];
      }
    }

    // 处理静音轨道或无音轨
    if (current_stream_id < 0) {
      static QByteArray silence(2048, 0); // 约23ms @ 44.1kHz 16-bit stereo
      emit audioReady(silence);
      std::this_thread::sleep_for(std::chrono::milliseconds(23));
      continue;
    }

    // 检测音轨是否变化，如果变化则重新初始化解码器和重采样器
    if (!actx || current_stream_id != last_audio_stream_id) {
      actx.reset(); // 释放旧的上下文
      if (swr_ctx)
        swr_free(&swr_ctx);

      AVStream *stream = fmt_ctx->streams[current_stream_id];
      const AVCodec *acodec = avcodec_find_decoder(stream->codecpar->codec_id);
      if (!acodec) {
        qWarning() << "Audio decoder not found";
        break;
      }

      AVCodecContext *new_actx = avcodec_alloc_context3(acodec);
      if (!new_actx) {
        qWarning() << "Failed to allocate audio decoder context";
        break;
      }
      actx.reset(new_actx);

      if (avcodec_parameters_to_context(actx.get(), stream->codecpar) < 0) {
        qWarning() << "Failed to copy audio decoder parameters";
        break;
      }
      if (avcodec_open2(actx.get(), acodec, nullptr) < 0) {
        qWarning() << "Failed to open audio decoder";
        break;
      }

      atime_base = stream->time_base;
      if (actx->channel_layout == 0)
        actx->channel_layout = av_get_default_channel_layout(actx->channels);

      swr_ctx = swr_alloc_set_opts(
          nullptr, av_get_default_channel_layout(OUT_CHANNELS), OUT_SAMPLE_FMT,
          OUT_SAMPLE_RATE, actx->channel_layout, actx->sample_fmt,
          actx->sample_rate, 0, nullptr);
      if (!swr_ctx || swr_init(swr_ctx) < 0) {
        qWarning() << "Failed to initialize audio resample context";
        swr_free(&swr_ctx);
        break;
      }

      last_audio_stream_id = current_stream_id;
      reset_decoder_and_sync_state(); // 音轨切换后必须重置所有状态
    }

    // 暂停处理
    if (m_pause) {
      std::unique_lock<std::mutex> lk(m_mutex);
      m_cond.wait(lk, [&] { return m_stop || !m_pause || m_seeking; });
      if (m_stop)
        break;
      if (!m_seeking) { // 从暂停中恢复时(且非跳转)，重置状态以避免卡顿
        reset_decoder_and_sync_state();
      }
    }

    // 跳转处理
    if (m_seeking) {
      int64_t ts = m_seekTarget * (AV_TIME_BASE / 1000);
      av_seek_frame(fmt_ctx.get(), -1, ts, AVSEEK_FLAG_BACKWARD);
      reset_decoder_and_sync_state(); // 跳转后调用集中的状态重置函数

      {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_audioSeekHandled = true;
        if (m_videoSeekHandled)
          m_seeking = false;
      }
      continue;
    }

    // 从文件中读取一个 packet
    if (av_read_frame(fmt_ctx.get(), pkt.get()) < 0) {
      m_eof = true;
      std::unique_lock<std::mutex> lk(m_mutex);
      m_cond.wait_for(lk, std::chrono::milliseconds(50),
                      [&] { return m_stop || m_seeking || m_eof == false; });
      if (m_stop)
        break;
      if (m_seeking)
        m_eof = false;
      continue;
    }

    // 如果不是我们想要的音频流，则丢弃
    if (pkt->stream_index != current_stream_id) {
      av_packet_unref(pkt.get());
      continue;
    }

    // 发送 packet 到解码器
    if (avcodec_send_packet(actx.get(), pkt.get()) < 0) {
      av_packet_unref(pkt.get());
      continue;
    }
    av_packet_unref(pkt.get()); // 发送后即可释放 packet

    // 从解码器接收解码后的 frame
    while (!m_stop && !m_seeking) {
      int ret = avcodec_receive_frame(actx.get(), frame.get());
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        break; // 需要更多 packet 或已到流末尾
      } else if (ret < 0) {
        // 解码出错
        break;
      }

      if (frame->nb_samples == 0)
        continue;

      // 计算 PTS (Presentation Timestamp) in milliseconds
      int64_t pts = frame->pts != AV_NOPTS_VALUE ? frame->pts
                                                 : frame->best_effort_timestamp;
      int64_t ms = (pts == AV_NOPTS_VALUE)
                       ? 0
                       : av_rescale_q(pts, atime_base, {1, 1000});
      if (ms < 0)
        continue;
      m_audioClockMs.store(ms);

      // --- 音频同步逻辑 (改进版，支持倍速平滑切换) ---
      double speed = m_playbackSpeed.load();

      // 检测速度变化，如果速度变化超过阈值，重置同步参考点
      static double last_speed = 1.0;
      bool speed_changed = fabs(speed - last_speed) > 0.1;
      if (speed_changed) {
        first_audio_frame = true; // 强制重置参考点
        last_speed = speed;
      }

      if (first_audio_frame) {
        audio_playback_start_time = clock::now();
        first_audio_pts = ms;
        first_audio_frame = false;
      } else {
        int64_t elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                clock::now() - audio_playback_start_time)
                .count();
        double diff_ms = ((ms - first_audio_pts) / speed) - elapsed_ms;

        // 简单的同步：如果音频播放得太快，就等待一下
        if (diff_ms > 10) { // 音频超前于时钟
          std::this_thread::sleep_for(
              std::chrono::milliseconds(static_cast<int>(diff_ms * 0.8)));
        }
      }
      // --- 同步逻辑结束 ---

      if (m_stop || m_seeking)
        break;

      // 音频重采样
      int out_nb = av_rescale_rnd(
          swr_get_delay(swr_ctx, actx->sample_rate) + frame->nb_samples,
          OUT_SAMPLE_RATE, actx->sample_rate, AV_ROUND_UP);
      if (out_nb > out_buf_samples) {
        if (out_buf)
          av_freep(&out_buf[0]);
        av_freep(&out_buf);
        av_samples_alloc_array_and_samples(&out_buf, nullptr, OUT_CHANNELS,
                                           out_nb, OUT_SAMPLE_FMT, 0);
        out_buf_samples = out_nb;
      }

      int converted_samples =
          swr_convert(swr_ctx, out_buf, out_nb, (const uint8_t **)frame->data,
                      frame->nb_samples);
      int data_size = av_samples_get_buffer_size(
          nullptr, OUT_CHANNELS, converted_samples, OUT_SAMPLE_FMT, 1);

      QByteArray pcm =
          QByteArray::fromRawData((const char *)out_buf[0], data_size);
      emit audioReady(pcm);
      emit positionChanged(ms);

      av_frame_unref(frame.get()); // 处理完一帧后立即释放
    }
  }

  // 清理循环内分配的资源
  if (out_buf) {
    av_freep(&out_buf[0]);
    av_freep(&out_buf);
  }
  if (swr_ctx) {
    swr_free(&swr_ctx);
  }
}

void FFMpegDecoder::setPlaybackSpeed(float speed) {
  // 限制播放速度范围在0.25-4.0之间
  float newSpeed = std::max(0.25f, std::min(speed, 4.0f));
  float oldSpeed = m_playbackSpeed.load();

  // 只有当速度真正变化时才更新和发送信号
  if (fabs(newSpeed - oldSpeed) > 0.01f) {
    m_playbackSpeed.store(newSpeed);
  }
}

float FFMpegDecoder::playbackSpeed() const {
    return m_playbackSpeed.load();
}