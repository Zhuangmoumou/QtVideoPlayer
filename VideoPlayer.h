#pragma once
#include <QAudioOutput>
#include <QElapsedTimer>
#include <QMap>
#include <QTimer>
#include <QWidget>
#include <ass/ass.h> // 新增：libass 头文件
#include <QFileSystemWatcher> // 新增
#include <QPushButton>
#include <QMenu>
#include <QAction>

#include "FFMpegDecoder.h"
#include "LyricRenderer.h"
#include "SubtitleRenderer.h"

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

  LyricRenderer *lyricRenderer = nullptr;
  SubtitleRenderer *subtitleRenderer = nullptr;

  // libass 相关
  ASS_Library *assLibrary = nullptr;
  ASS_Renderer *assRenderer = nullptr;

  QImage currentFrame;
  QString videoInfoLabel;

  // 进度条和媒体信息显示控制
  bool showOverlayBar = false;
  QTimer *overlayBarTimer = nullptr;

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

  void seekByDelta(int dx);
  void showOverlay(bool visible);
  void drawOverlayBar(QPainter &p);
  void drawProgressBar(QPainter &p);
  void drawSubtitlesAndLyrics(QPainter &p);
  void showOverlayBarForSeconds(int seconds);

  QFileSystemWatcher *screenStatusWatcher;

  // 新增：错误提示
  QString errorMessage;
  QTimer *errorShowTimer = nullptr;

  // 新增：歌词渐变相关
  qreal lyricOpacity = 1.0;
  QElapsedTimer lyricFadeTimer;

  LyricManager *lyricManager;
  SubtitleManager *subtitleManager;

  // 音轨/视频轨道切换按钮和菜单
  QPushButton *trackButton = nullptr;
  QMenu *audioMenu = nullptr;
  QMenu *videoMenu = nullptr;
};