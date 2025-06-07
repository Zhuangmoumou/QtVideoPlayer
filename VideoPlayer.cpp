#include "VideoPlayer.h"
#include "qdebug.h"
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMediaMetaData>
#include <QMouseEvent>
#include <QPainter>
#include <QTextStream>
#include <taglib/attachedpictureframe.h>
#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/id3v2tag.h>
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
  connect(scrollTimer, &QTimer::timeout, this, [this]() {
    scrollOffset += 2;
    update();
  });
  scrollOffset = 0;
}

VideoPlayer::~VideoPlayer() {
  decoder->stop();
  audioOutput->stop();
  scrollTimer->stop();
}

void VideoPlayer::play(const QString &path) {
  loadCoverAndLyrics(path);
  currentLyricIndex = 0; // 播放新文件时重置歌词下标

  // 新增：加载同名 srt 字幕
  subtitles.clear();
  currentSubtitleIndex = -1;
  QString srtPath = QFileInfo(path).absolutePath() + "/" +
                    QFileInfo(path).completeBaseName() + ".srt";
  if (QFile::exists(srtPath)) {
    loadSrtSubtitle(srtPath);
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
  showOverlayBarForSeconds(5); // 播放时显示 overlay

  // 启动滚动定时器
  scrollTimer->start();
}

void VideoPlayer::loadCoverAndLyrics(const QString &path) {
  lyrics.clear();
  coverArt = QPixmap();
  bool embeddedLyricLoaded = false;
  QString suffix = QFileInfo(path).suffix().toLower();

  if (suffix == "mp3") {
    // MP3: TagLib 读取封面和内嵌歌词
    TagLib::FileRef f(path.toUtf8().constData());
    if (!f.isNull() && f.tag()) {
      auto *id3 = dynamic_cast<TagLib::ID3v2::Tag *>(f.file()->tag());
      if (id3) {
        // 读取封面
        auto frames = id3->frameListMap()["APIC"];
        if (!frames.isEmpty()) {
          auto pic = static_cast<TagLib::ID3v2::AttachedPictureFrame *>(
              frames.front());
          coverArt.loadFromData((const uchar *)pic->picture().data(),
                                pic->picture().size());
        }
        // 读取内嵌歌词（USLT 帧）
        auto usltFrames = id3->frameListMap()["USLT"];
        if (!usltFrames.isEmpty()) {
          for (auto *frame : usltFrames) {
            auto *uslt =
                dynamic_cast<TagLib::ID3v2::UnsynchronizedLyricsFrame *>(frame);
            if (uslt) {
              QString lyricText =
                  QString::fromStdWString(uslt->text().toWString());
              QRegExp rx("\\[(\\d+):(\\d+\\.\\d+)\\]");
              QStringList lines = lyricText.split('\n');
              QMap<qint64, QString> lyricMap;
              for (const QString &line : lines) {
                int pos = 0;
                QList<qint64> times;
                while ((pos = rx.indexIn(line, pos)) != -1) {
                  qint64 t = rx.cap(1).toInt() * 60000 +
                             int(rx.cap(2).toDouble() * 1000);
                  times.append(t);
                  pos += rx.matchedLength();
                }
                QString text = line;
                text.remove(QRegExp("(\\[\\d+:\\d+\\.\\d+\\])+"));
                text = text.trimmed();
                if (!times.isEmpty() && !text.isEmpty()) {
                  for (qint64 t : times) {
                    if (lyricMap.contains(t)) {
                      lyricMap[t] += "\n" + text;
                    } else {
                      lyricMap[t] = text;
                    }
                  }
                }
              }
              if (lyricMap.isEmpty() && !lyricText.trimmed().isEmpty()) {
                lyrics.append({0, lyricText.trimmed()});
              } else {
                for (auto it = lyricMap.constBegin(); it != lyricMap.constEnd();
                     ++it) {
                  lyrics.append({it.key(), it.value()});
                }
                std::sort(lyrics.begin(), lyrics.end(),
                          [](const LyricLine &a, const LyricLine &b) {
                            return a.time < b.time;
                          });
              }
              embeddedLyricLoaded = !lyrics.isEmpty();
              if (embeddedLyricLoaded)
                break;
            }
          }
        }
      }
    }
  } else if (suffix == "flac") {
    // FLAC: TagLib 读取 Vorbis Comment 的 LYRICS 字段
    TagLib::FileRef f(path.toUtf8().constData());
    if (!f.isNull() && f.file()) {
      auto *flacFile = dynamic_cast<TagLib::FLAC::File *>(f.file());
      if (flacFile && flacFile->xiphComment()) {
        auto *comment = flacFile->xiphComment();
        if (comment->contains("LYRICS")) {
          // 修正：先 toString，再转 QString
          QString lyricText = QString::fromUtf8(
              comment->fieldListMap()["LYRICS"].toString().toCString(true));
          QRegExp rx("\\[(\\d+):(\\d+\\.\\d+)\\]");
          QStringList lines = lyricText.split('\n');
          QMap<qint64, QString> lyricMap;
          for (const QString &line : lines) {
            int pos = 0;
            QList<qint64> times;
            while ((pos = rx.indexIn(line, pos)) != -1) {
              qint64 t =
                  rx.cap(1).toInt() * 60000 + int(rx.cap(2).toDouble() * 1000);
              times.append(t);
              pos += rx.matchedLength();
            }
            QString text = line;
            text.remove(QRegExp("(\\[\\d+:\\d+\\.\\d+\\])+"));
            text = text.trimmed();
            if (!times.isEmpty() && !text.isEmpty()) {
              for (qint64 t : times) {
                if (lyricMap.contains(t)) {
                  lyricMap[t] += "\n" + text;
                } else {
                  lyricMap[t] = text;
                }
              }
            }
          }
          if (lyricMap.isEmpty() && !lyricText.trimmed().isEmpty()) {
            lyrics.append({0, lyricText.trimmed()});
          } else {
            for (auto it = lyricMap.constBegin(); it != lyricMap.constEnd();
                 ++it) {
              lyrics.append({it.key(), it.value()});
            }
            std::sort(lyrics.begin(), lyrics.end(),
                      [](const LyricLine &a, const LyricLine &b) {
                        return a.time < b.time;
                      });
          }
          embeddedLyricLoaded = !lyrics.isEmpty();
        }
        // 读取封面（FLAC 封面可选实现，略）
      }
    }
  }
  // 其他类型或未读取到内嵌歌词，尝试读取同名 .lrc
  if (!embeddedLyricLoaded) {
    QString lrc = QFileInfo(path).absolutePath() + "/" +
                  QFileInfo(path).completeBaseName() + ".lrc";
    if (QFile::exists(lrc)) {
      QFile f(lrc);
      if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        QMap<qint64, QString> lyricMap;
        while (!in.atEnd()) {
          QString line = in.readLine();
          QRegExp rx("\\[(\\d+):(\\d+\\.\\d+)\\]");
          int pos = 0;
          QList<qint64> times;
          while ((pos = rx.indexIn(line, pos)) != -1) {
            qint64 t =
                rx.cap(1).toInt() * 60000 + int(rx.cap(2).toDouble() * 1000);
            times.append(t);
            pos += rx.matchedLength();
          }
          QString text = line;
          text.remove(QRegExp("(\\[\\d+:\\d+\\.\\d+\\])+"));
          text = text.trimmed();
          if (!times.isEmpty() && !text.isEmpty()) {
            for (qint64 t : times) {
              if (lyricMap.contains(t)) {
                lyricMap[t] += "\n" + text;
              } else {
                lyricMap[t] = text;
              }
            }
          }
        }
        for (auto it = lyricMap.constBegin(); it != lyricMap.constEnd(); ++it) {
          lyrics.append({it.key(), it.value()});
        }
        std::sort(lyrics.begin(), lyrics.end(),
                  [](const LyricLine &a, const LyricLine &b) {
                    return a.time < b.time;
                  });
      }
    }
  }
}

void VideoPlayer::loadSrtSubtitle(const QString &path) {
  subtitles.clear();
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return;
  QTextStream in(&f);
  QString line;
  QRegExp timeRx(R"((\d+):(\d+):(\d+),(\d+)\s*-->\s*(\d+):(\d+):(\d+),(\d+))");
  while (!in.atEnd()) {
    // 跳过序号行
    line = in.readLine();
    if (line.trimmed().isEmpty())
      continue;
    // 时间行
    QString timeLine = line;
    if (!timeRx.exactMatch(timeLine))
      continue;
    qint64 start = timeRx.cap(1).toInt() * 3600000 +
                   timeRx.cap(2).toInt() * 60000 +
                   timeRx.cap(3).toInt() * 1000 + timeRx.cap(4).toInt();
    qint64 end = timeRx.cap(5).toInt() * 3600000 +
                 timeRx.cap(6).toInt() * 60000 + timeRx.cap(7).toInt() * 1000 +
                 timeRx.cap(8).toInt();
    // 字幕内容
    QString text;
    while (!in.atEnd()) {
      QString t = in.readLine();
      if (t.trimmed().isEmpty())
        break;
      if (!text.isEmpty())
        text += "\n";
      text += t;
    }
    subtitles.append({start, end, text});
  }
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
  // 修正歌词下标同步，支持快退
  int idx = 0;
  while (idx + 1 < lyrics.size() && lyrics[idx + 1].time <= pts) {
    idx++;
  }
  if (currentLyricIndex != idx) {
    lastLyricIndex = currentLyricIndex;
    currentLyricIndex = idx;
    lyricFadeTimer.restart();
    lyricOpacity = 0.0;
    overlayTimer->start();
  }

  // SRT 字幕同步
  int subIdx = -1;
  for (int i = 0; i < subtitles.size(); ++i) {
    if (pts >= subtitles[i].startTime && pts <= subtitles[i].endTime) {
      subIdx = i;
      break;
    }
  }
  if (currentSubtitleIndex != subIdx) {
    currentSubtitleIndex = subIdx;
    update();
  }

  update(); // 确保进度条和歌词/字幕刷新
}

void VideoPlayer::mousePressEvent(QMouseEvent *e) {
  pressed = true;
  pressPos = e->pos();
  pressTimer.start();
}

void VideoPlayer::mouseReleaseEvent(QMouseEvent *) {
  pressed = false;
  if (isSeeking) {
    isSeeking = false;
    decoder->seek(currentPts);
    // seeking 时一直显示 overlay，不自动隐藏
    overlayBarTimer->stop();
    showOverlayBar = true;
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
      showOverlayBarForSeconds(5);
    }
  }
}

void VideoPlayer::mouseDoubleClickEvent(QMouseEvent *) { close(); }

void VideoPlayer::mouseMoveEvent(QMouseEvent *e) {
  if (!pressed)
    return;
  // 仅在暂停状态下允许滑动 seek
  if (!decoder->isPaused())
    return;
  int dx = e->pos().x() - pressPos.x();
  if (qAbs(dx) > 20) {
    isSeeking = true;
    seekByDelta(dx);
  }
}

void VideoPlayer::seekByDelta(int dx) {
  // 仅在暂停状态下允许 seek
  if (!decoder->isPaused())
    return;
  // 每滑动 100px，快进/后退 5 秒
  qint64 delta = dx * 50; // ms per px
  qint64 target = qBound<qint64>(0, currentPts + delta, duration);
  currentPts = target;
}

void VideoPlayer::resizeEvent(QResizeEvent *) {
  // 移除 videoWidget 相关代码
}

void VideoPlayer::paintEvent(QPaintEvent *) {
  QPainter p(this);
  // 填充黑色背景，防止残影
  p.fillRect(rect(), Qt::black);

  // 如果有视频帧则绘制视频帧，否则绘制封面
  if (!currentFrame.isNull()) {
    // 保持比例居中显示
    QSize imgSize = currentFrame.size();
    QSize widgetSize = size();
    imgSize.scale(widgetSize, Qt::KeepAspectRatio);
    QRect targetRect(QPoint(0, 0), imgSize);
    targetRect.moveCenter(rect().center());
    p.drawImage(targetRect, currentFrame);
  } else if (!coverArt.isNull()) {
    // 居中绘制封面
    int coverW = qMin(width() / 2, 240);
    int coverH = coverW;
    int x = (width() - coverW) / 2;
    int y = (height() - coverH) / 2;
    p.drawPixmap(x, y, coverW, coverH, coverArt);
  }

  // 只在 showOverlayBar 为 true 时绘制媒体信息和进度条
  if (showOverlayBar) {
    // 左上角视频信息标签
    if (!videoInfoLabel.isEmpty() || !currentFileName.isEmpty()) {
      QFont infoFont("Microsoft YaHei", overlayFontSize - 2, QFont::Bold); // 使用统一字号
      p.setFont(infoFont);
      p.setPen(Qt::white);
      QRect infoRect = QRect(10, 10, width() / 3, 20);
      p.setBrush(QColor(0, 0, 0, 128));
      p.setRenderHint(QPainter::Antialiasing, true);
      p.drawRoundedRect(infoRect.adjusted(-4, -2, 4, 2), 6, 6);

      // 拼接文件名和媒体信息
      QString infoText = currentFileName;
      if (!videoInfoLabel.isEmpty()) {
        infoText += "  |  " + videoInfoLabel;
      }

      // 滚动显示
      QFontMetrics fm(infoFont);
      int textWidth = fm.horizontalAdvance(infoText);
      int rectWidth = infoRect.width() - 10;
      int x = infoRect.left() + 5;
      int y = infoRect.top();
      int availableWidth = rectWidth;

      if (textWidth > availableWidth) {
        int offset = scrollOffset % (textWidth + 40);
        int drawX = x - offset;
        p.setClipRect(infoRect.adjusted(2, 2, -2, -2));
        p.drawText(drawX, y + infoRect.height() - 8, infoText);
        // 循环补尾
        if (textWidth - offset < availableWidth) {
          p.drawText(drawX + textWidth + 40, y + infoRect.height() - 8,
                     infoText);
        }
        p.setClipping(false);
      } else {
        p.drawText(infoRect, Qt::AlignLeft | Qt::AlignVCenter, infoText);
      }
    }

    // 进度条
    double pct = duration > 0 ? double(currentPts) / duration : 0;
    int barMargin = 20;
    int barWidth = width() - barMargin * 2;
    int barHeight = 10;
    int barY = height() - 30;
    QRect bar(barMargin, barY, barWidth, barHeight);

    p.setPen(Qt::white);
    p.setBrush(Qt::NoBrush);
    p.drawRect(bar);

    p.fillRect(QRect(bar.left() + 1, bar.top() + 1,
                     int((bar.width() - 2) * pct), bar.height() - 2),
               Qt::white);
  }

  // 歌词
  QRect lyricRect = rect().adjusted(0, height() - 70, 0, -10);
  // SRT 字幕绘制（优先于歌词，样式同歌词）
  {
    // 渐变动画（出现和消失）
    static int lastSubIdx = -2;
    static QElapsedTimer subFadeTimer;
    static bool fadingOut = false;
    static qreal fadeOpacity = 1.0;
    static QString lastSubText;
    qreal opacity = 1.0;
    QString subText;

    if (currentSubtitleIndex >= 0 && currentSubtitleIndex < subtitles.size()) {
      subText = subtitles[currentSubtitleIndex].text;
      if (lastSubIdx != currentSubtitleIndex) {
        // 字幕切换，启动淡入
        subFadeTimer.restart();
        fadingOut = false;
        lastSubIdx = currentSubtitleIndex;
        fadeOpacity = 1.0;
        lastSubText = subText;
      }
    } else {
      // 字幕消失，启动淡出
      if (!fadingOut && lastSubIdx != -1) {
        subFadeTimer.restart();
        fadingOut = true;
        fadeOpacity = 1.0;
      }
      subText = lastSubText;
    }

    if (!subText.isEmpty()) {
      if (!fadingOut) {
        // 淡入
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
        // 淡出
        if (subFadeTimer.isValid()) {
          qint64 elapsed = subFadeTimer.elapsed();
          if (elapsed < 400) {
            opacity = fadeOpacity * (1.0 - elapsed / 400.0);
            QTimer::singleShot(16, this, [this] { update(); });
          } else {
            opacity = 0.0;
            fadingOut = false;
            lastSubIdx = -1;
            lastSubText.clear();
          }
        }
      }

      if (opacity > 0.01) {
        QFont subFont("Microsoft YaHei", overlayFontSize, QFont::Bold); // 使用统一字号
        p.setFont(subFont);

        QRect textRect = p.fontMetrics().boundingRect(
            lyricRect, Qt::AlignHCenter | Qt::AlignVCenter, subText);
        textRect = textRect.marginsAdded(QMargins(18, 8, 18, 8));
        textRect.moveCenter(lyricRect.center());

        // 半透明黑色背景
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        QColor bgColor(0, 0, 0, int(180 * opacity));
        p.setPen(Qt::NoPen);
        p.setBrush(bgColor);
        p.drawRoundedRect(textRect, 12, 12);
        p.restore();

        // 白色文字
        p.save();
        QColor textColor(255, 255, 255, int(255 * opacity));
        p.setPen(textColor);
        p.drawText(textRect, Qt::AlignHCenter | Qt::AlignVCenter, subText);
        p.restore();
      }
      // SRT 绘制后直接 return，避免歌词和字幕重叠
      if (opacity > 0.01)
        return;
    }
  }

  // 歌词渐变动画
  qreal opacity = lyricOpacity;
  if (lyricFadeTimer.isValid()) {
    qint64 elapsed = lyricFadeTimer.elapsed();
    if (elapsed < 400) {
      opacity = qMin(1.0, elapsed / 400.0);
    } else {
      opacity = 1.0;
    }
  }
  // 当前歌词
  if (currentLyricIndex < lyrics.size()) {
    QFont lyricFont("Microsoft YaHei", overlayFontSize, QFont::Bold); // 使用统一字号
    p.setFont(lyricFont);

    // Youtube 风格：黑色半透明背景，白色文字
    QString lyricText = lyrics[currentLyricIndex].text;
    QRect textRect = p.fontMetrics().boundingRect(
        lyricRect, Qt::AlignHCenter | Qt::AlignVCenter, lyricText);
    textRect = textRect.marginsAdded(QMargins(18, 8, 18, 8));
    textRect.moveCenter(lyricRect.center());

    // 半透明黑色背景
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor bgColor(0, 0, 0, int(180 * opacity));
    p.setPen(Qt::NoPen);
    p.setBrush(bgColor);
    p.drawRoundedRect(textRect, 12, 12);
    p.restore();

    // 白色文字
    p.save();
    QColor textColor(255, 255, 255, int(255 * opacity));
    p.setPen(textColor);
    p.drawText(textRect, Qt::AlignHCenter | Qt::AlignVCenter, lyricText);
    p.restore();
  }
  // 上一行歌词淡出（可选）
  if (lastLyricIndex >= 0 && lastLyricIndex < lyrics.size() &&
      lyricOpacity < 1.0) {
    QFont lyricFont("Microsoft YaHei", overlayFontSize, QFont::Bold); // 使用统一字号
    p.setFont(lyricFont);

    QString lyricText = lyrics[lastLyricIndex].text;
    QRect textRect = p.fontMetrics().boundingRect(
        lyricRect, Qt::AlignHCenter | Qt::AlignVCenter, lyricText);
    textRect = textRect.marginsAdded(QMargins(18, 8, 18, 8));
    textRect.moveCenter(lyricRect.center());

    // 半透明黑色背景
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor bgColor(0, 0, 0, int(180 * (1.0 - opacity) * 0.7));
    p.setPen(Qt::NoPen);
    p.setBrush(bgColor);
    p.drawRoundedRect(textRect, 12, 12);
    p.restore();

    // 白色文字
    p.save();
    QColor textColor(255, 255, 255, int(255 * (1.0 - opacity) * 0.7));
    p.setPen(textColor);
    p.drawText(textRect, Qt::AlignHCenter | Qt::AlignVCenter, lyricText);
    p.restore();
  }
}

void VideoPlayer::updateOverlay() {
  // 歌词渐变动画刷新
  if (lyricFadeTimer.isValid()) {
    qint64 elapsed = lyricFadeTimer.elapsed();
    if (elapsed < 400) {
      lyricOpacity = qMin(1.0, elapsed / 400.0);
      update();
    } else {
      lyricOpacity = 1.0;
      lastLyricIndex = -1;
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
