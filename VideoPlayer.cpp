#include "VideoPlayer.h"
#include "LyricManager.h"
#include "SubtitleManager.h"
#include "qglobal.h"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMediaMetaData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess> // 新增
#include <QTextStream>
#include <ass/ass.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/tag.h>
#include <taglib/unsynchronizedlyricsframe.h>
#include <taglib/xiphcomment.h>

VideoPlayer::VideoPlayer(QWidget *parent) : QWidget(parent) {
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

  // Overlay 更新
  overlayTimer = new QTimer(this);
  overlayTimer->setInterval(200);
  connect(overlayTimer, &QTimer::timeout, this, &VideoPlayer::updateOverlay);

  // 新增：进度条和媒体信息显示定时器
  overlayBarTimer = new QTimer(this);
  overlayBarTimer->setSingleShot(true);
  connect(overlayBarTimer, &QTimer::timeout, this, [this]() {
    showOverlayBar = false;
    update();
  });
  showOverlayBar = false;

  // 文件名滚动
  scrollTimer = new QTimer(this);
  scrollTimer->setInterval(40); // 25fps
  scrollPause = false;
  scrollPauseTimer = new QTimer(this);
  scrollPauseTimer->setSingleShot(true);
  connect(scrollTimer, &QTimer::timeout, this, [this]() {
    if (scrollPause)
      return;
    scrollOffset += 2;
    update();
  });
  connect(scrollPauseTimer, &QTimer::timeout, this, [this]() {
    scrollPause = false;
    scrollOffset = 0;
    update();
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
        update();
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
}

VideoPlayer::~VideoPlayer() {
  decoder->stop();
  audioOutput->stop();
  scrollTimer->stop();
  subtitleCheckTimer->stop();

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
  update();
  scrollTimer->start();
  subtitleCheckTimer->start();
}

void VideoPlayer::onFrame(const QImage &frame) {
  currentFrame = frame.copy();
  update();
}

void VideoPlayer::onAudioData(const QByteArray &data) { audioIO->write(data); }

void VideoPlayer::onPositionChanged(qint64 pts) {
  // 拖动 seeking 时不更新进度条进度
  if (isSeeking) {
    return;
  }
  currentPts = pts;

  lyricManager->updateLyricsIndex(pts);
  subtitleManager->updateSubtitleIndex(pts);

  update(); // 确保进度条和歌词/字幕刷新
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
    update();
  } else {
    decoder->togglePause();
    // 判断当前是否为暂停状态
    if (decoder->isPaused()) {
      // 暂停时一直显示 overlay
      overlayBarTimer->stop();
      showOverlayBar = true;
      update();
    } else {
      // 播放时显示 5 秒 overlay
      showOverlayBar = true;
      overlayBarTimer->start(5 * 1000);
      update();
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
  update();
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

  if (!currentFrame.isNull()) {
    QSize imgSize = currentFrame.size();
    QSize widgetSize = size();
    imgSize.scale(widgetSize, Qt::KeepAspectRatio);
    QRect targetRect(QPoint(0, 0), imgSize);
    targetRect.moveCenter(rect().center());
    p.drawImage(targetRect, currentFrame);
  }

  if (showOverlayBar) {
    drawOverlayBar(p);
  }

  drawSubtitlesAndLyrics(p);

  if (subtitleManager->hasAss() && subtitleManager->getAssTrack() && assRenderer) {
    drawAssSubtitles(p);
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
  drawSrtSubtitles(p, lyricRect);
  drawLyrics(p, lyricRect);
}

void VideoPlayer::drawSrtSubtitles(QPainter &p, const QRect &lyricRect) {
  // 渐变动画（出现和消失）
  static int lastSubIdx = -2;
  static QElapsedTimer subFadeTimer;
  static bool fadingOut = false;
  static qreal fadeOpacity = 1.0;
  static QString lastSubText;
  qreal opacity = 1.0;
  QString subText;
  const auto &subs = subtitleManager->getSubtitles();
  int curIdx = subtitleManager->getCurrentSubtitleIndex();
  if (curIdx >= 0 && curIdx < subs.size()) {
    subText = subs[curIdx].text;
    if (lastSubIdx != curIdx) {
      subFadeTimer.restart();
      fadingOut = false;
      lastSubIdx = curIdx;
      fadeOpacity = 1.0;
      lastSubText = subText;
    }
  } else {
    // 只有在字幕刚刚消失时才触发一次淡出动画
    if (!fadingOut && lastSubIdx >= 0) {
      subFadeTimer.restart();
      fadingOut = true;
      fadeOpacity = 1.0;
      subText = lastSubText;
    } else if (fadingOut && lastSubIdx >= 0) {
      subText = lastSubText;
    } else {
      // 没有字幕且没有淡出动画需要播放
      return;
    }
  }
  if (!subText.isEmpty()) {
    if (!fadingOut) {
      if (subFadeTimer.isValid()) {
        qint64 elapsed = subFadeTimer.elapsed();
        if (elapsed < 400) {
          opacity = qMin(1.0, elapsed / 400.0);
          QTimer::singleShot(16, this, [this] { update(); });
        } else {
          opacity = 1.0;
        }
      }
      fadeOpacity = opacity;
    } else {
      if (subFadeTimer.isValid()) {
        qint64 elapsed = subFadeTimer.elapsed();
        if (elapsed < 400) {
          opacity = fadeOpacity * (1.0 - elapsed / 400.0);
          QTimer::singleShot(16, this, [this] { update(); });
        } else {
          opacity = 0.0;
          fadingOut = false;
          lastSubIdx = -2; // 彻底重置，防止多次淡出
          lastSubText.clear();
        }
      }
    }
    if (opacity > 0.01) {
      QFont subFont("Microsoft YaHei", overlayFontSize - 2, QFont::Bold);
      p.setFont(subFont);
      QRect textRect = p.fontMetrics().boundingRect(
          lyricRect, Qt::AlignHCenter | Qt::AlignVCenter, subText);
      textRect = textRect.marginsAdded(QMargins(10, 8, 10, 8));
      textRect.moveCenter(lyricRect.center());
      p.save();
      p.setRenderHint(QPainter::Antialiasing, true);
      QColor bgColor(0, 0, 0, int(180 * opacity));
      p.setPen(Qt::NoPen);
      p.setBrush(bgColor);
      p.drawRoundedRect(textRect, 12, 12);
      p.restore();
      p.save();
      QColor textColor(255, 255, 255, int(255 * opacity));
      p.setPen(textColor);
      p.drawText(textRect, Qt::AlignHCenter | Qt::AlignVCenter, subText);
      p.restore();
    }
    if (opacity > 0.01)
      return;
  }
}

void VideoPlayer::drawLyrics(QPainter &p, const QRect &lyricRect) {
  qreal opacity = lyricOpacity;
  if (lyricFadeTimer.isValid()) {
    qint64 elapsed = lyricFadeTimer.elapsed();
    if (elapsed < 400) {
      opacity = qMin(1.0, elapsed / 400.0);
    } else {
      opacity = 1.0;
    }
  }
  const auto &lyrics = lyricManager->getLyrics();
  int curIdx = lyricManager->getCurrentLyricIndex();
  int lastIdx = lyricManager->getLastLyricIndex();
  if (curIdx < lyrics.size()) {
    QFont lyricFont("Microsoft YaHei", overlayFontSize - 2, QFont::Bold);
    p.setFont(lyricFont);
    QString lyricText = lyrics[curIdx].text;
    QRect textRect = p.fontMetrics().boundingRect(
        lyricRect, Qt::AlignHCenter | Qt::AlignVCenter, lyricText);
    textRect = textRect.marginsAdded(QMargins(10, 8, 10, 8));
    textRect.moveCenter(lyricRect.center());
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor bgColor(0, 0, 0, int(180 * opacity));
    p.setPen(Qt::NoPen);
    p.setBrush(bgColor);
    p.drawRoundedRect(textRect, 12, 12);
    p.restore();
    p.save();
    QColor textColor(255, 255, 255, int(255 * opacity));
    p.setPen(textColor);
    p.drawText(textRect, Qt::AlignHCenter | Qt::AlignVCenter, lyricText);
    p.restore();
  }
  // 上一行歌词淡出（可选）
  if (lastIdx >= 0 && lastIdx < lyrics.size() && lyricOpacity < 1.0) {
    QFont lyricFont("Microsoft YaHei", overlayFontSize - 2, QFont::Bold);
    p.setFont(lyricFont);
    QString lyricText = lyrics[lastIdx].text;
    QRect textRect = p.fontMetrics().boundingRect(
        lyricRect, Qt::AlignHCenter | Qt::AlignVCenter, lyricText);
    textRect = textRect.marginsAdded(QMargins(10, 8, 10, 8));
    textRect.moveCenter(lyricRect.center());
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor bgColor(0, 0, 0, int(180 * (1.0 - opacity) * 0.7));
    p.setPen(Qt::NoPen);
    p.setBrush(bgColor);
    p.drawRoundedRect(textRect, 12, 12);
    p.restore();
    p.save();
    QColor textColor(255, 255, 255, int(255 * (1.0 - opacity) * 0.7));
    p.setPen(textColor);
    p.drawText(textRect, Qt::AlignHCenter | Qt::AlignVCenter, lyricText);
    p.restore();
  }
}

void VideoPlayer::drawAssSubtitles(QPainter &p) {
  if (subtitleManager->hasAss() && subtitleManager->getAssTrack() && assRenderer) {
    int w = width(), h = height();
    ass_set_frame_size(assRenderer, w, h);
    long long now = currentPts;
    int detectChange = 0;
    ASS_Image *img =
        ass_render_frame(assRenderer, subtitleManager->getAssTrack(), now, &detectChange);
    for (; img; img = img->next) {
      QImage qimg((const uchar *)img->bitmap, img->w, img->h, img->stride,
                  QImage::Format_Alpha8);
      QColor color;
      color.setRgba(qRgba((img->color >> 24) & 0xFF, (img->color >> 16) & 0xFF,
                          (img->color >> 8) & 0xFF, 255 - (img->color & 0xFF)));
      QImage colored(qimg.size(), QImage::Format_ARGB32_Premultiplied);
      colored.fill(Qt::transparent);
      QPainter qp(&colored);
      qp.setCompositionMode(QPainter::CompositionMode_Source);
      qp.fillRect(qimg.rect(), color);
      qp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
      qp.drawImage(0, 0, qimg);
      qp.end();
      p.drawImage(img->dst_x, img->dst_y, colored);
    }
  }
}

void VideoPlayer::updateOverlay() {
  if (lyricFadeTimer.isValid()) {
    qint64 elapsed = lyricFadeTimer.elapsed();
    if (elapsed < 400) {
      lyricOpacity = qMin(1.0, elapsed / 400.0);
      update();
    } else {
      lyricOpacity = 1.0;
      // lastLyricIndex = -1; // lastLyricIndex已由LyricManager管理
      overlayTimer->stop();
      update();
    }
  }
}

void VideoPlayer::showOverlayBarForSeconds(int seconds) {
  showOverlayBar = true;
  overlayBarTimer->start(seconds * 1000);
  update();
}