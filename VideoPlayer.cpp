#include "VideoPlayer.h"
#include "LyricManager.h"
#include "SubtitleManager.h"
#include "LyricRenderer.h"
#include "SubtitleRenderer.h"
#include "qelapsedtimer.h"
#include "qglobal.h"
#include <QDateTime>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMediaMetaData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QTextStream>
#include <QSharedPointer>
#include <ass/ass.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/tag.h>
#include <taglib/unsynchronizedlyricsframe.h>
#include <taglib/xiphcomment.h>
#include <QTimer>
#include <QPushButton>
#include <QMenu>
#include <QAction>

VideoPlayer::VideoPlayer(QWidget *parent) : QWidget(parent), lastScrollUpdateTime(0), updatePending(false), lastUpdateTime(0) {
  setAttribute(Qt::WA_AcceptTouchEvents);
  setWindowFlags(Qt::FramelessWindowHint);

  // AudioOutput
  QAudioFormat format;
  format.setSampleRate(44100);
  format.setChannelCount(2);
  format.setSampleSize(16);
  format.setCodec("audio/pcm");
  format.setByteOrder(QAudioFormat::LittleEndian);
  format.setSampleType(QAudioFormat::SignedInt);

  audioOutput =
      new QAudioOutput(QAudioDeviceInfo::defaultOutputDevice(), format);
  audioIO = audioOutput->start();

  // Decoder
  decoder = new FFMpegDecoder(this);
  connect(decoder, &FFMpegDecoder::frameReady, this, &VideoPlayer::onFrame);
  connect(decoder, &FFMpegDecoder::audioReady, this, &VideoPlayer::onAudioData);
  connect(decoder, &FFMpegDecoder::durationChanged, this,
          [&](qint64 d) { duration = d; });
  connect(decoder, &FFMpegDecoder::positionChanged, this,
          &VideoPlayer::onPositionChanged);
  // 新增：错误提示
  errorShowTimer = new QTimer(this);
  errorShowTimer->setSingleShot(true);
  connect(errorShowTimer, &QTimer::timeout, this, [this]() {
    errorMessage.clear();
    scheduleUpdate();
  });
  connect(decoder, &FFMpegDecoder::errorOccurred, this, [this](const QString &msg) {
    errorMessage = msg;
    errorShowTimer->start(3000); // 显示 3 秒
    scheduleUpdate();
  });

  // Overlay 更新
  overlayTimer = new QTimer(this);
  overlayTimer->setInterval(200);
  connect(overlayTimer, &QTimer::timeout, this, &VideoPlayer::updateOverlay);

  // 进度条和媒体信息显示定时器
  overlayBarTimer = new QTimer(this);
  overlayBarTimer->setSingleShot(true);
  connect(overlayBarTimer, &QTimer::timeout, this, [this]() {
    showOverlayBar = false;
    scheduleUpdate();
  });
  showOverlayBar = false;

  trackButtonTimer = new QElapsedTimer();
  trackButtonTimer->start();
  
  // 帧率控制定时器 - 60fps (约 16.67ms)
  frameRateTimer = new QTimer(this);
  frameRateTimer->setInterval(16); // ~60fps
  connect(frameRateTimer, &QTimer::timeout, this, [this]() {
    if (updatePending) {
      updatePending = false;
      QWidget::update();
    }
  });
  frameRateTimer->start();

  // 文件名滚动
  scrollTimer = new QTimer(this);
  scrollTimer->setInterval(80); // 降低到12.5fps，原来是25fps
  scrollPause = false;
  scrollPauseTimer = new QTimer(this);
  scrollPauseTimer->setSingleShot(true);

  // 上次滚动更新时间
  lastScrollUpdateTime = 0;

  connect(scrollTimer, &QTimer::timeout, this, [this]() {
    if (scrollPause)
      return;

    // 获取当前时间
    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();

    // 增加滚动偏移
    scrollOffset += 2;

    // 只有当显示覆盖栏时或者距离上次更新超 200ms 时才刷新界面
    if (showOverlayBar || currentTime - lastScrollUpdateTime > 200) {
      lastScrollUpdateTime = currentTime;
      scheduleUpdate();
    }
  });

  connect(scrollPauseTimer, &QTimer::timeout, this, [this]() {
    scrollPause = false;
    scrollOffset = 0;
    scheduleUpdate();
  });

  scrollOffset = 0;
  // 新增：字幕定时检查定时器
  subtitleCheckTimer = new QTimer(this);
  subtitleCheckTimer->setInterval(1000);
  connect(subtitleCheckTimer, &QTimer::timeout, this, [this]() {
    const auto &subs = subtitleManager->getSubtitles();
    int curIdx = subtitleManager->getCurrentSubtitleIndex();
    if (!subs.isEmpty()) {
      bool found = false;
      for (const auto &subtitle : subs) {
        if (currentPts >= subtitle.startTime && currentPts <= subtitle.endTime) {
          found = true;
          break;
        }
      }
      if (!found && curIdx != -1) {
        // 通过subtitleManager重置下标
        subtitleManager->updateSubtitleIndex(-1);
        scheduleUpdate();
      }
    }
  });

  // 新增：libass 初始化
  assLibrary = ass_library_init();
  if (assLibrary) {
    assRenderer = ass_renderer_init(assLibrary);
    if (assRenderer) {
      ass_set_fonts(assRenderer, nullptr, "Microsoft YaHei", 1, nullptr, 1);
    }
  }
  // assTrack和hasAssSubtitle已由SubtitleManager管理

  // 新增：屏幕状态文件监听
  screenStatusWatcher = new QFileSystemWatcher(this);
  QString screenStatusPath = "/tmp/screen_status";
  QString screenStatusDir = QFileInfo(screenStatusPath).absolutePath();
  screenStatusWatcher->addPath(screenStatusDir);
  connect(screenStatusWatcher, &QFileSystemWatcher::directoryChanged, this,
          [screenStatusPath]() {
            if (QFile::exists(screenStatusPath)) {
              QTimer::singleShot(3000, []() {
                QProcess::execute("ubus",
                                  QStringList()
                                      << "call" << "eq_drc_process.output.rpc"
                                      << "control" << R"({"action":"Open"})");
              });
            }
          });

  lyricManager = new LyricManager();
  subtitleManager = new SubtitleManager();
  lyricRenderer = new LyricRenderer(lyricManager);
  subtitleRenderer = new SubtitleRenderer(subtitleManager);

  // 轨道切换按钮和菜单
  trackButton = new QPushButton("轨道", this);
  trackButton->setGeometry(10, 40, 60, 28);
  trackButton->setStyleSheet("background:rgba(30,30,30,180);color:white;border-radius:8px;");
  trackButton->raise();
  connect(trackButton, &QPushButton::clicked, this, [this]() {
    QMenu menu;
    QActionGroup *audioGroup = new QActionGroup(&menu);
    audioGroup->setExclusive(true);
    int acnt = decoder->audioTrackCount();
    for (int i = 0; i < acnt; ++i) {
      QAction *act = menu.addAction(decoder->audioTrackName(i));
      act->setCheckable(true);
      act->setChecked(decoder->currentAudioTrack() == i);
      audioGroup->addAction(act);
      connect(act, &QAction::triggered, this, [this, i]() {
        decoder->setAudioTrack(i);
        errorMessage = tr("切换音轨: %1").arg(decoder->audioTrackName(i));
        errorShowTimer->start(2000);
        scheduleUpdate();
      });
    }
    // QAction *muteAct = menu.addAction(tr("静音轨道"));
    // muteAct->setCheckable(true);
    // muteAct->setChecked(decoder->currentAudioTrack() == -1);
    // audioGroup->addAction(muteAct);
    // connect(muteAct, &QAction::triggered, this, [this]() {
    //   decoder->setAudioTrack(-1);
    //   errorMessage = tr("切换音轨: 静音轨道");
    //   errorShowTimer->start(2000);
    //   scheduleUpdate();
    // });
    menu.addSeparator();
    QActionGroup *videoGroup = new QActionGroup(&menu);
    videoGroup->setExclusive(true);
    int vcnt = decoder->videoTrackCount();
    for (int i = 0; i < vcnt; ++i) {
      QAction *act = menu.addAction(decoder->videoTrackName(i));
      act->setCheckable(true);
      act->setChecked(decoder->currentVideoTrack() == i);
      videoGroup->addAction(act);
      connect(act, &QAction::triggered, this, [this, i]() {
        decoder->setVideoTrack(i);
        errorMessage = tr("切换视频轨道: %1").arg(decoder->videoTrackName(i));
        errorShowTimer->start(2000);
        scheduleUpdate();
      });
    }
    QAction *noVideoAct = menu.addAction(tr("无视频轨道"));
    noVideoAct->setCheckable(true);
    noVideoAct->setChecked(decoder->currentVideoTrack() == -1);
    videoGroup->addAction(noVideoAct);
    connect(noVideoAct, &QAction::triggered, this, [this]() {
      decoder->setVideoTrack(-1);
      errorMessage = tr("切换视频轨道: 无视频轨道");
      errorShowTimer->start(2000);
      scheduleUpdate();
    });
    menu.exec(trackButton->mapToGlobal(QPoint(0, trackButton->height())));
  });
}

VideoPlayer::~VideoPlayer() {
  decoder->stop();
  audioOutput->stop();
  scrollTimer->stop();
  subtitleCheckTimer->stop();
  frameRateTimer->stop();

  // 新增：libass 资源释放
  // 由SubtitleManager自动管理ASS资源，无需手动释放assTrack/hasAssSubtitle
  if (assRenderer) {
    ass_renderer_done(assRenderer);
    assRenderer = nullptr;
  }
  if (assLibrary) {
    ass_library_done(assLibrary);
    assLibrary = nullptr;
  }

  // 关闭音频输出（使用 startDetached，避免不执行）
  // QProcess::startDetached("ubus", QStringList() << "call" <<
  // "eq_drc_process.output.rpc" << "control" << R"({"action":"Close"})");

  delete lyricRenderer;
  delete subtitleRenderer;
  delete lyricManager;
  delete subtitleManager;
}

void VideoPlayer::play(const QString &path) {
  // 启动音频输出
  QProcess::execute("ubus", QStringList()
                                << "call" << "eq_drc_process.output.rpc"
                                << "control" << R"({"action":"Open"})");

  lyricManager->loadLyrics(path);
  subtitleManager->reset();

  // 新增：加载字幕（支持模糊匹配）
  QString basePath =
      QFileInfo(path).absolutePath() + "/" + QFileInfo(path).completeBaseName();
  QString assPath = basePath + ".ass";
  QString srtPath = basePath + ".srt";
  QString subtitlePath;
  if (QFile::exists(assPath)) {
    subtitleManager->loadAssSubtitle(assPath, assLibrary, assRenderer);
  } else if (QFile::exists(srtPath)) {
    subtitleManager->loadSrtSubtitle(srtPath);
  } else if (subtitleManager->findSimilarSubtitle(path, subtitlePath)) {
    if (subtitlePath.endsWith(".ass", Qt::CaseInsensitive)) {
      subtitleManager->loadAssSubtitle(subtitlePath, assLibrary, assRenderer);
    } else if (subtitlePath.endsWith(".srt", Qt::CaseInsensitive)) {
      subtitleManager->loadSrtSubtitle(subtitlePath);
    }
  }

  // 读取视频/音频信息
  videoInfoLabel.clear();
  AVFormatContext *fmt_ctx = nullptr;
  if (avformat_open_input(&fmt_ctx, path.toUtf8().constData(), nullptr,
                          nullptr) == 0) {
    if (avformat_find_stream_info(fmt_ctx, nullptr) >= 0) {
      int vid_idx = -1, aid_idx = -1;
      for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        AVCodecParameters *p = fmt_ctx->streams[i]->codecpar;
        if (p->codec_type == AVMEDIA_TYPE_VIDEO && vid_idx < 0)
          vid_idx = i;
        if (p->codec_type == AVMEDIA_TYPE_AUDIO && aid_idx < 0)
          aid_idx = i;
      }
      if (vid_idx >= 0) {
        AVCodecParameters *vpar = fmt_ctx->streams[vid_idx]->codecpar;
        videoInfoLabel +=
            QString("视频: %1x%2  ").arg(vpar->width).arg(vpar->height);
      }
      if (aid_idx >= 0) {
        AVCodecParameters *apar = fmt_ctx->streams[aid_idx]->codecpar;
        videoInfoLabel += QString("音频: %1Hz %2ch  ")
                              .arg(apar->sample_rate)
                              .arg(apar->channels);
      }
      if (fmt_ctx->duration > 0) {
        int sec = fmt_ctx->duration / AV_TIME_BASE;
        int min = sec / 60;
        sec = sec % 60;
        videoInfoLabel += QString("时长: %1:%2  ")
                              .arg(min, 2, 10, QChar('0'))
                              .arg(sec, 2, 10, QChar('0'));
      }
    }
    avformat_close_input(&fmt_ctx);
  }

  // 新增：保存文件名
  currentFileName = QFileInfo(path).fileName();

  // 重置滚动
  scrollOffset = 0;
  scrollTimer->stop();
  decoder->start(path);
  show();
  showOverlayBar = true;
  overlayBarTimer->start(5 * 1000);
  scheduleUpdate();
  scrollTimer->start();
  subtitleCheckTimer->start();
}

void VideoPlayer::onFrame(const QSharedPointer<QImage> &frame) {
  currentFrame = frame;
  scheduleUpdate();
}

void VideoPlayer::onAudioData(const QByteArray &data) { audioIO->write(data); }

void VideoPlayer::onPositionChanged(qint64 pts) {
  // 拖动 seeking 时不更新进度条进度
  if (isSeeking) {
    return;
  }

  // 检查 pts 变化是否足够大以至于需要更新UI
  // 只有当时间变化超过100ms或者是显示覆盖栏时才更新进度条
  bool needUpdate = showOverlayBar || abs(currentPts - pts) > 100;

  currentPts = pts;

  // 记录当前歌词索引
  int oldLyricIndex = lyricManager->getCurrentLyricIndex();

  // 更新歌词和字幕索引
  lyricManager->updateLyricsIndex(pts);
  subtitleManager->updateSubtitleIndex(pts);

  // 如果歌词索引发生变化，重置淡入淡出计时器并启动动画
  if (oldLyricIndex != lyricManager->getCurrentLyricIndex()) {
    lyricFadeTimer.restart();
    overlayTimer->start();
    needUpdate = true;  // 歌词变化时需要更新 UI
  }

  // 只在需要时更新 UI
  if (needUpdate) {
    scheduleUpdate();
  }
}

void VideoPlayer::mousePressEvent(QMouseEvent *e) {
  pressed = true;
  pressPos = e->pos();
  // pressTimer.start();
  // 移除长按关闭窗口逻辑
}

void VideoPlayer::mouseReleaseEvent(QMouseEvent *) {
  pressed = false;
  if (isSeeking) {
    isSeeking = false;
    // seek 前检查 duration 是否有效
    if (duration > 0 && currentPts >= 0 && currentPts <= duration) {
      decoder->seek(currentPts);
    }
    showOverlayBar = true;
    overlayBarTimer->start(5 * 1000);
    scheduleUpdate();
  } else {
    decoder->togglePause();
    // 判断当前是否为暂停状态
    if (decoder->isPaused()) {
      // 暂停时一直显示 overlay
      overlayBarTimer->stop();
      showOverlayBar = true;
      scheduleUpdate();
    } else {
      // 播放时显示 5 秒 overlay
      showOverlayBar = true;
      overlayBarTimer->start(5 * 1000);
      scheduleUpdate();
    }
  }
}

// 改为双击关闭窗口
void VideoPlayer::mouseDoubleClickEvent(QMouseEvent *) { close(); }

void VideoPlayer::mouseMoveEvent(QMouseEvent *e) {
  if (!pressed || e == nullptr)
    return;

  int dx = e->pos().x() - pressPos.x();
  isSeeking = true;
  seekByDelta(dx);

  overlayBarTimer->stop();
  showOverlayBar = true;
  scheduleUpdate();
}

void VideoPlayer::seekByDelta(int dx) {
  // 动态调整每像素对应的毫秒数，随视频时长自适应
  // 例如：每像素调整为总时长的1/500，限制最小20ms，最大2000ms
  qint64 msPerPx = 20;
  if (duration > 0) {
    msPerPx = qBound<qint64>(20, duration / 5000, 2000);
  }
  qint64 delta = dx * msPerPx;
  qint64 target = qBound<qint64>(0, currentPts + delta, duration);
  currentPts = target;
}

void VideoPlayer::resizeEvent(QResizeEvent *) {
  // 移除 videoWidget 相关代码
}

void VideoPlayer::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.fillRect(rect(), Qt::black);
  if (currentFrame && !currentFrame->isNull()) {
    QSize imgSize = currentFrame->size();
    QSize widgetSize = size();
    imgSize.scale(widgetSize, Qt::KeepAspectRatio);
    QRect targetRect(QPoint(0, 0), imgSize);
    targetRect.moveCenter(rect().center());
    p.drawImage(targetRect, *currentFrame);
  }
  if (!errorMessage.isEmpty()) {
    QFont errFont("Microsoft YaHei", overlayFontSize + 4, QFont::Bold);
    p.setFont(errFont);
    QString msg = errorMessage;
    QFontMetrics fm(errFont);
    int textWidth = fm.horizontalAdvance(msg);
    int textHeight = fm.height();
    QRect boxRect((width() - textWidth) / 2 - 30, (height() - textHeight) / 2 - 16, textWidth + 60, textHeight + 32);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 180));
    p.drawRoundedRect(boxRect, 18, 18);
    p.setPen(QColor(220, 40, 40));
    p.drawText(boxRect, Qt::AlignCenter, msg);
  }
  if (trackButtonTimer->elapsed() > 100) {
      trackButton->setVisible(showOverlayBar);
      trackButton->setEnabled(true);
      trackButton->raise();
      trackButtonTimer->restart();
  }
  if (showOverlayBar) {
    drawOverlayBar(p);
  }
  drawSubtitlesAndLyrics(p);
  if (subtitleManager->hasAss() && subtitleManager->getAssTrack() && assRenderer) {
    subtitleRenderer->setAssRenderer(assRenderer);
    subtitleRenderer->drawAssSubtitles(p, width(), height(), currentPts);
  }
}

void VideoPlayer::drawOverlayBar(QPainter &p) {
  if (!videoInfoLabel.isEmpty() || !currentFileName.isEmpty()) {
    QFont infoFont("Microsoft YaHei", overlayFontSize, QFont::Bold);
    p.setFont(infoFont);
    p.setPen(Qt::white);
    QRect infoRect = QRect(10, 10, width() / 1.5, 22);
    p.setBrush(QColor(0, 0, 0, 128));
    p.setRenderHint(QPainter::Antialiasing, true);
    p.drawRoundedRect(infoRect.adjusted(-4, -2, 4, 2), 6, 6);

    QString infoText = currentFileName;
    if (!videoInfoLabel.isEmpty()) {
      infoText += "  |  " + videoInfoLabel;
    }

    QFontMetrics fm(infoFont);
    int textWidth = fm.horizontalAdvance(infoText);
    int rectWidth = infoRect.width() - 10;
    int x = infoRect.left() + 5;
    int y = infoRect.top();
    int availableWidth = rectWidth;

    if (textWidth > availableWidth) {
      int totalScroll = textWidth + 40;
      int offset = scrollOffset % totalScroll;
      int drawX = x - offset;
      p.setClipRect(infoRect.adjusted(2, 2, -2, -2));
      p.drawText(drawX, y + infoRect.height() - 8, infoText);
      if (textWidth - offset < availableWidth) {
        p.drawText(drawX + totalScroll, y + infoRect.height() - 8, infoText);
      }
      p.setClipping(false);

      if (!scrollPause && offset + 2 >= totalScroll - 2) {
        scrollPause = true;
        scrollPauseTimer->start(3000);
      }
    } else {
      p.drawText(infoRect, Qt::AlignLeft | Qt::AlignVCenter, infoText);
    }
  }

  drawProgressBar(p);
}

void VideoPlayer::drawProgressBar(QPainter &p) {
  double pct = duration > 0 ? double(currentPts) / duration : 0;
  int barMargin = 20;
  int barWidth = width() - barMargin * 2;
  int barHeight = 10;
  int barY = height() - 30;
  QRect bar(barMargin, barY, barWidth, barHeight);
  int radius = barHeight / 2;
  p.setRenderHint(QPainter::Antialiasing, true);

  QPainterPath shadowPath;
  shadowPath.addRoundedRect(bar.adjusted(-2, 2, 2, 6), radius + 2, radius + 2);
  QColor shadowColor(0, 0, 0, 80);
  p.save();
  p.setPen(Qt::NoPen);
  p.setBrush(shadowColor);
  p.drawPath(shadowPath);
  p.restore();

  p.setPen(Qt::NoPen);
  p.setBrush(QColor(255, 255, 255, 60));
  p.drawRoundedRect(bar, radius, radius);

  int playedWidth = int(bar.width() * pct);
  if (playedWidth > 0) {
    QRect playedRect = QRect(bar.left(), bar.top(), playedWidth, bar.height());
    p.setBrush(Qt::white);
    p.drawRoundedRect(playedRect, radius, radius);
    if (playedWidth < bar.height()) {
      QPainterPath path;
      path.moveTo(bar.left(), bar.top() + bar.height() / 2.0);
      path.arcTo(bar.left(), bar.top(), bar.height(), bar.height(), 90, 180);
      path.closeSubpath();
      p.fillPath(path, Qt::white);
    }
  }

  p.setPen(QPen(Qt::white, 1));
  p.setBrush(Qt::NoBrush);
  p.drawRoundedRect(bar, radius, radius);
}

void VideoPlayer::drawSubtitlesAndLyrics(QPainter &p) {
  QRect lyricRect = rect().adjusted(0, height() - 70, 0, -10);
  subtitleRenderer->drawSrtSubtitles(p, lyricRect, overlayFontSize, currentPts);
  lyricRenderer->drawLyrics(p, lyricRect, overlayFontSize, lyricOpacity, lyricFadeTimer);
}

void VideoPlayer::updateOverlay() {
  if (lyricFadeTimer.isValid()) {
    qint64 elapsed = lyricFadeTimer.elapsed();
    if (elapsed < 600) { // 延长一点淡入时间，让效果更明显
      // 非线性淡入效果，开始较慢，然后加速
      double newOpacity = qMin(1.0, 0.2 + (elapsed / 600.0) * 0.8);

      // 只有当不透明度变化超过 0.03 时才更新界面
      if (qAbs(newOpacity - lyricOpacity) > 0.03) {
        lyricOpacity = newOpacity;
        scheduleUpdate();
      }
    } else {
      // 淡入完成
      if (lyricOpacity < 1.0) {
        lyricOpacity = 1.0;
        scheduleUpdate();
      }
      // 保持计时器有效，以便LyricRenderer能够使用它计算淡出效果
      // 但停止定时更新，因为淡入已完成
      overlayTimer->stop();
    }
  }
}

void VideoPlayer::showOverlayBarForSeconds(int seconds) {
  showOverlayBar = true;
  overlayBarTimer->start(seconds * 1000);
}
void VideoPlayer::scheduleUpdate() {
  // 获取当前时间戳
  qint64 currentTime = QDateTime::currentMSecsSinceEpoch();

  // 如果距离上次更新时间超过 16ms (约 60 fps)，立即更新界面
  if (currentTime - lastUpdateTime > 16) {
    lastUpdateTime = currentTime;
    QWidget::update();
  } else if (currentTime - lastUpdateTime > 5) {
    // 只有当间隔超过 5ms 时才设置标记，避免过于频繁的更新请求
    updatePending = true;
  }
  // 小于 5ms 的更新请求会被忽略，防止频繁的 UI 更新
}