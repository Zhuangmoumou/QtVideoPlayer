#pragma once
#include <QImage>
#include <QObject>
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

signals:
  void frameReady(const QImage &img);
  void audioReady(const QByteArray &pcm);
  void durationChanged(qint64 ms);
  void positionChanged(qint64 ms);

private:
  // 线程与同步
  std::thread m_thread;
  std::atomic<bool> m_stop{false};
  std::atomic<bool> m_pause{false};
  std::atomic<bool> m_seeking{false};
  std::atomic<qint64> m_seekTarget{0};
  std::mutex m_mutex;
  std::condition_variable m_cond;

  // 播放参数
  QString m_path;

  // 解码主循环
  void decodeLoop();
};