#include "VideoPlayer.h"
#include "qdebug.h"
#include <QMouseEvent>
#include <QPainter>
#include <QFile>
#include <QDir>
#include <QMediaMetaData>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/unsynchronizedlyricsframe.h>
#include <taglib/flacfile.h>
#include <taglib/xiphcomment.h>
#include <QTextStream>
#include <QDebug>

VideoPlayer::VideoPlayer(QWidget *parent)
    : QWidget(parent)
{
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

    audioOutput = new QAudioOutput(QAudioDeviceInfo::defaultOutputDevice(), format);
    audioIO = audioOutput->start();

    // Decoder
    decoder = new FFMpegDecoder(this);
    connect(decoder, &FFMpegDecoder::frameReady, this, &VideoPlayer::onFrame);
    connect(decoder, &FFMpegDecoder::audioReady, this, &VideoPlayer::onAudioData);
    connect(decoder, &FFMpegDecoder::durationChanged, this, [&](qint64 d){ duration = d; });
    connect(decoder, &FFMpegDecoder::positionChanged, this, &VideoPlayer::onPositionChanged);

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

VideoPlayer::~VideoPlayer()
{
    decoder->stop();
    audioOutput->stop();
    scrollTimer->stop();
}

void VideoPlayer::play(const QString &path)
{
    loadCoverAndLyrics(path);
    currentLyricIndex = 0; // 播放新文件时重置歌词下标

    // 读取视频/音频信息
    videoInfoLabel.clear();
    AVFormatContext *fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, path.toUtf8().constData(), nullptr, nullptr) == 0) {
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
                videoInfoLabel += QString("视频: %1x%2  ").arg(vpar->width).arg(vpar->height);
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

void VideoPlayer::loadCoverAndLyrics(const QString &path)
{
    lyrics.clear();
    coverArt = QPixmap();
    bool embeddedLyricLoaded = false;
    QString suffix = QFileInfo(path).suffix().toLower();

    if (suffix == "mp3") {
        // MP3: TagLib 读取封面和内嵌歌词
        TagLib::FileRef f(path.toUtf8().constData());
        if (!f.isNull() && f.tag()) {
            auto *id3 = dynamic_cast<TagLib::ID3v2::Tag*>(f.file()->tag());
            if (id3) {
                // 读取封面
                auto frames = id3->frameListMap()["APIC"];
                if (!frames.isEmpty()) {
                    auto pic = static_cast<TagLib::ID3v2::AttachedPictureFrame*>(frames.front());
                    coverArt.loadFromData((const uchar*)pic->picture().data(), pic->picture().size());
                }
                // 读取内嵌歌词（USLT 帧）
                auto usltFrames = id3->frameListMap()["USLT"];
                if (!usltFrames.isEmpty()) {
                    for (auto *frame : usltFrames) {
                        auto *uslt = dynamic_cast<TagLib::ID3v2::UnsynchronizedLyricsFrame*>(frame);
                        if (uslt) {
                            QString lyricText = QString::fromStdWString(uslt->text().toWString());
                            QRegExp rx("\\[(\\d+):(\\d+\\.\\d+)\\]");
                            QStringList lines = lyricText.split('\n');
                            QMap<qint64, QString> lyricMap;
                            for (const QString &line : lines) {
                                int pos = 0;
                                QList<qint64> times;
                                while ((pos = rx.indexIn(line, pos)) != -1) {
                                    qint64 t = rx.cap(1).toInt()*60000 + int(rx.cap(2).toDouble()*1000);
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
                                for (auto it = lyricMap.constBegin(); it != lyricMap.constEnd(); ++it) {
                                    lyrics.append({it.key(), it.value()});
                                }
                                std::sort(lyrics.begin(), lyrics.end(), [](const LyricLine &a, const LyricLine &b){ return a.time < b.time; });
                            }
                            embeddedLyricLoaded = !lyrics.isEmpty();
                            if (embeddedLyricLoaded) break;
                        }
                    }
                }
            }
        }
    } else if (suffix == "flac") {
        // FLAC: TagLib 读取 Vorbis Comment 的 LYRICS 字段
        TagLib::FileRef f(path.toUtf8().constData());
        if (!f.isNull() && f.file()) {
            auto *flacFile = dynamic_cast<TagLib::FLAC::File*>(f.file());
            if (flacFile && flacFile->xiphComment()) {
                auto *comment = flacFile->xiphComment();
                if (comment->contains("LYRICS")) {
                    // 修正：先 toString，再转 QString
                    QString lyricText = QString::fromUtf8(comment->fieldListMap()["LYRICS"].toString().toCString(true));
                    QRegExp rx("\\[(\\d+):(\\d+\\.\\d+)\\]");
                    QStringList lines = lyricText.split('\n');
                    QMap<qint64, QString> lyricMap;
                    for (const QString &line : lines) {
                        int pos = 0;
                        QList<qint64> times;
                        while ((pos = rx.indexIn(line, pos)) != -1) {
                            qint64 t = rx.cap(1).toInt()*60000 + int(rx.cap(2).toDouble()*1000);
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
                        for (auto it = lyricMap.constBegin(); it != lyricMap.constEnd(); ++it) {
                            lyrics.append({it.key(), it.value()});
                        }
                        std::sort(lyrics.begin(), lyrics.end(), [](const LyricLine &a, const LyricLine &b){ return a.time < b.time; });
                    }
                    embeddedLyricLoaded = !lyrics.isEmpty();
                }
                // 读取封面（FLAC 封面可选实现，略）
            }
        }
    }
    // 其他类型或未读取到内嵌歌词，尝试读取同名 .lrc
    if (!embeddedLyricLoaded) {
        QString lrc = QFileInfo(path).absolutePath() + "/" + QFileInfo(path).completeBaseName() + ".lrc";
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
                        qint64 t = rx.cap(1).toInt()*60000 + int(rx.cap(2).toDouble()*1000);
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
                std::sort(lyrics.begin(), lyrics.end(), [](const LyricLine &a, const LyricLine &b){ return a.time < b.time; });
            }
        }
    }
}

void VideoPlayer::onFrame(const QImage &frame)
{
    currentFrame = frame.copy();
    update();
}

void VideoPlayer::onAudioData(const QByteArray &data)
{
    audioIO->write(data);
}

void VideoPlayer::onPositionChanged(qint64 pts)
{
    currentPts = pts;
    // 修正歌词下标同步，支持快退
    int idx = 0;
    while (idx+1 < lyrics.size() && lyrics[idx+1].time <= pts) {
        idx++;
    }
    if (currentLyricIndex != idx) {
        lastLyricIndex = currentLyricIndex;
        currentLyricIndex = idx;
        lyricFadeTimer.restart();
        lyricOpacity = 0.0;
        overlayTimer->start();
    }
    update(); // 确保进度条和歌词刷新
}

void VideoPlayer::mousePressEvent(QMouseEvent *e)
{
    pressed = true;
    pressPos = e->pos();
    pressTimer.start();
}

void VideoPlayer::mouseReleaseEvent(QMouseEvent *)
{
    pressed = false;
    if (isSeeking) {
        isSeeking = false;
        decoder->seek(currentPts);
        showOverlayBarForSeconds(5); // 拖动进度条后显示 overlay
    } else {
        decoder->togglePause();
        showOverlayBarForSeconds(5); // 暂停/播放切换时显示 overlay
    }
}

void VideoPlayer::mouseDoubleClickEvent(QMouseEvent*)
{
    close();
}

void VideoPlayer::mouseMoveEvent(QMouseEvent *e)
{
    if (!pressed) return;
    int dx = e->pos().x() - pressPos.x();
    if (qAbs(dx) > 20) {
        isSeeking = true;
        seekByDelta(dx);
    }
}

void VideoPlayer::seekByDelta(int dx)
{
    // 每滑动 100px，快进/后退 5 秒
    qint64 delta = dx * 50; // ms per px
    qint64 target = qBound<qint64>(0, currentPts + delta, duration);
    currentPts = target;
}

void VideoPlayer::resizeEvent(QResizeEvent *)
{
    // 移除 videoWidget 相关代码
}

void VideoPlayer::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    // 填充黑色背景，防止残影
    p.fillRect(rect(), Qt::black);

    // 如果有视频帧则绘制视频帧，否则绘制封面
    if (!currentFrame.isNull()) {
        // 保持比例居中显示
        QSize imgSize = currentFrame.size();
        QSize widgetSize = size();
        imgSize.scale(widgetSize, Qt::KeepAspectRatio);
        QRect targetRect(QPoint(0,0), imgSize);
        targetRect.moveCenter(rect().center());
        p.drawImage(targetRect, currentFrame);
    } else if (!coverArt.isNull()) {
        // 居中绘制封面
        int coverW = qMin(width()/2, 240);
        int coverH = coverW;
        int x = (width() - coverW) / 2;
        int y = (height() - coverH) / 2;
        p.drawPixmap(x, y, coverW, coverH, coverArt);
    }

    // 只在 showOverlayBar 为 true 时绘制媒体信息和进度条
    if (showOverlayBar) {
        // 左上角视频信息标签
        if (!videoInfoLabel.isEmpty() || !currentFileName.isEmpty()) {
            QFont infoFont("Microsoft YaHei", 10, QFont::Bold); // 修改为微软雅黑
            p.setFont(infoFont);
            p.setPen(Qt::white);
            QRect infoRect = QRect(10, 10, width()/1.5, 24);
            p.setBrush(QColor(0,0,0,128));
            p.setRenderHint(QPainter::Antialiasing, true);
            p.drawRoundedRect(infoRect.adjusted(-4,-2,4,2), 6, 6);

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
                p.setClipRect(infoRect.adjusted(2,2,-2,-2));
                p.drawText(drawX, y + infoRect.height() - 8, infoText);
                // 循环补尾
                if (textWidth - offset < availableWidth) {
                    p.drawText(drawX + textWidth + 40, y + infoRect.height() - 8, infoText);
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

        p.fillRect(QRect(bar.left() + 1, bar.top() + 1, int((bar.width() - 2) * pct), bar.height() - 2), Qt::white);
    }

    // 歌词
    QRect lyricRect = rect().adjusted(0, height()-70, 0, -10);
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
        QFont lyricFont("Microsoft YaHei", 12, QFont::Bold); // 字号更大
        p.setFont(lyricFont);

        // Youtube 风格：黑色半透明背景，白色文字
        QString lyricText = lyrics[currentLyricIndex].text;
        QRect textRect = p.fontMetrics().boundingRect(lyricRect, Qt::AlignHCenter | Qt::AlignVCenter, lyricText);
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
    if (lastLyricIndex >= 0 && lastLyricIndex < lyrics.size() && lyricOpacity < 1.0) {
        QFont lyricFont("Microsoft YaHei", 12, QFont::Bold);
        p.setFont(lyricFont);

        QString lyricText = lyrics[lastLyricIndex].text;
        QRect textRect = p.fontMetrics().boundingRect(lyricRect, Qt::AlignHCenter | Qt::AlignVCenter, lyricText);
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

void VideoPlayer::updateOverlay()
{
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

void VideoPlayer::showOverlayBarForSeconds(int seconds)
{
    showOverlayBar = true;
    overlayBarTimer->start(seconds * 1000);
    update();
}
