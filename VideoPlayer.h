#pragma once
#include <QAudioOutput>
#include <QElapsedTimer>
#include <QMap>
#include <QTimer>
#include <QWidget>
#include <ass/ass.h> // 新增：libass 头文件
#include <QFileSystemWatcher> // 新增

#include "FFMpegDecoder.h"

class VideoPlayer : public QWidget {
  Q_OBJECT
public:
  explicit VideoPlayer(QWidget *parent = nullptr);
  ~VideoPlayer();
  void play(const QString &path);

protected:
  // 手势/点击处理（双击关闭窗口）
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
  // QElapsedTimer pressTimer;
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

  // SRT/ASS 字幕支持
  struct SubtitleLine {
    qint64 startTime;
    qint64 endTime;
    QString text;
    QString assRaw; // 新增：原始 ass 行（用于样式渲染）
  };
  QList<SubtitleLine> subtitles;
  int currentSubtitleIndex = -1;
  void loadSrtSubtitle(const QString &path);
  void loadAssSubtitle(const QString &path); // 新增：加载 ass 字幕

  // libass 相关
  ASS_Library *assLibrary = nullptr;
  ASS_Renderer *assRenderer = nullptr;
  ASS_Track *assTrack = nullptr;
  bool hasAssSubtitle = false;

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
  // 滚动停顿
  bool scrollPause = false;
  QTimer *scrollPauseTimer = nullptr;

  // 统一 overlay 字号
  int overlayFontSize = 10;

  QTimer *subtitleCheckTimer = nullptr;

  void loadCoverAndLyrics(const QString &path);
  void seekByDelta(int dx);
  void showOverlay(bool visible);

  QFileSystemWatcher *screenStatusWatcher;
};