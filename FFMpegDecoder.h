#pragma once
#include <QImage>
#include <QObject>
#include <QSharedPointer>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
}

class FFMpegDecoder : public QObject {
  Q_OBJECT
public:
  explicit FFMpegDecoder(QObject *parent = nullptr);
  ~FFMpegDecoder();

  // path: 文件路径
  void start(const QString &path); // 移除 rate 参数
  void stop();
  void seek(qint64 ms);
  void togglePause();
  bool isPaused() const; // 新增：判断是否暂停

  // 音轨切换
  void setAudioTrack(int index); // index=-1 为静音
  int audioTrackCount() const;
  int currentAudioTrack() const;
  QString audioTrackName(int idx) const;

  // 视频轨道切换
  void setVideoTrack(int index); // index=-1 为无视频
  int videoTrackCount() const;
  int currentVideoTrack() const;
  QString videoTrackName(int idx) const;

  // 倍速支持
  void setPlaybackSpeed(float speed);
  float playbackSpeed() const;

signals:
  void frameReady(const QSharedPointer<QImage> &img);
  void audioReady(const QByteArray &pcm);
  void durationChanged(qint64 ms);
  void positionChanged(qint64 ms);
  void errorOccurred(const QString &message); // 新增：错误信号
  void seekCompleted(); // 新增：seek完成信号

private:
  // 线程与同步
  std::thread m_videoThread;
  std::thread m_audioThread;
  std::atomic<bool> m_stop{false};
  std::atomic<bool> m_pause{false};
  std::atomic<bool> m_seeking{false};
  std::atomic<qint64> m_seekTarget{0};
  std::mutex m_mutex;
  std::condition_variable m_cond;

  // seek 同步标志
  bool m_videoSeekHandled = false;
  bool m_audioSeekHandled = false;

  // 播放结束标志
  std::atomic<bool> m_eof{false}; // 新增

  // 音频时钟（ms）
  std::atomic<qint64> m_audioClockMs{0};

  // 播放参数
  QString m_path;

  // 解码主循环
  void videoDecodeLoop();
  void audioDecodeLoop();

  int m_audioTrackIndex = 0;                       // -1为静音
  mutable std::vector<int> m_audioStreamIndices;   // 存储所有音频流索引
  mutable std::vector<QString> m_audioStreamNames; // 存储音轨描述

  int m_videoTrackIndex = 0; // -1为无视频
  mutable std::vector<int> m_videoStreamIndices;
  mutable std::vector<QString> m_videoStreamNames;

  // 倍速支持
  std::atomic<float> m_playbackSpeed{1.0f};
};