#pragma once
#include <QAudioOutput>
#include <QMap>
#include <QTimer>
#include <QWidget>
#include <QElapsedTimer>

#include "FFMpegDecoder.h"

class VideoPlayer : public QWidget {
  Q_OBJECT
public:
  explicit VideoPlayer(QWidget *parent = nullptr);
  ~VideoPlayer();
  void play(const QString &path);

protected:
  // 手势/点击处理
  void mousePressEvent(QMouseEvent *e) override;
  void mouseReleaseEvent(QMouseEvent *e) override;
  void mouseDoubleClickEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;

  void resizeEvent(QResizeEvent *e) override;
  void paintEvent(QPaintEvent *e) override;

private slots:
  void onFrame(const QImage &frame);
  void onAudioData(const QByteArray &data);
  void onPositionChanged(qint64 pts);
  void updateOverlay();

private:
  QAudioOutput *audioOutput;
  QIODevice *audioIO;
  FFMpegDecoder *decoder;
  QTimer *overlayTimer;

  // 音频输出参数（补充声明）
  int audioSampleRate = 44100;
  int audioChannels = 2;
  int audioSampleSize = 16;

  // 状态管理
  bool pressed = false;
  QPoint pressPos;
  QElapsedTimer pressTimer;
  bool isSeeking = false;
  qint64 duration = 0;
  qint64 currentPts = 0;

  // 专辑封面与歌词
  QPixmap coverArt;
  struct LyricLine {
    qint64 time;
    QString text;
  };
  QList<LyricLine> lyrics;
  int currentLyricIndex = 0;
  // 歌词动画
  qreal lyricOpacity = 1.0;
  int lastLyricIndex = -1;
  QElapsedTimer lyricFadeTimer;

  QImage currentFrame;
  QString videoInfoLabel;

  // 进度条和媒体信息显示控制
  bool showOverlayBar = false;
  QTimer *overlayBarTimer = nullptr;
  void showOverlayBarForSeconds(int seconds);

  // 文件名滚动
  QString currentFileName;
  int scrollOffset = 0;
  QTimer *scrollTimer = nullptr;

  void loadCoverAndLyrics(const QString &path);
  void seekByDelta(int dx);
  void showOverlay(bool visible);
};