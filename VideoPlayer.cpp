#include "VideoPlayer.h"
#include <QMouseEvent>
#include <QPainter>
#include <QFile>
#include <QDir>
#include <QMediaMetaData>
#include <iostream>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
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
}

VideoPlayer::~VideoPlayer()
{
    decoder->stop();
    audioOutput->stop();
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
                videoInfoLabel += QString("时长: %1:%2")
                    .arg(min, 2, 10, QChar('0'))
                    .arg(sec, 2, 10, QChar('0'));
            }
        }
        avformat_close_input(&fmt_ctx);
    }

    decoder->start(path);
    show();
}

void VideoPlayer::loadCoverAndLyrics(const QString &path)
{
    // 使用 TagLib 读取封面
    TagLib::FileRef f(path.toUtf8().constData());
    if (!f.isNull() && f.tag()) {
        auto *id3 = dynamic_cast<TagLib::ID3v2::Tag*>(f.file()->tag());
        if (id3) {
            auto frames = id3->frameListMap()["APIC"];
            if (!frames.isEmpty()) {
                auto pic = static_cast<TagLib::ID3v2::AttachedPictureFrame*>(frames.front());
                coverArt.loadFromData((const uchar*)pic->picture().data(), pic->picture().size());
            }
        }
    }
    // 加载同名 .lrc
    QString lrc = QFileInfo(path).absolutePath() + "/" + QFileInfo(path).completeBaseName() + ".lrc";
    if (QFile::exists(lrc)) {
        QFile f(lrc);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            lyrics.clear();
            QTextStream in(&f);
            QMap<qint64, QString> lyricMap; // 用于合并同一时间戳的多行歌词
            while (!in.atEnd()) {
                QString line = in.readLine();
                QRegExp rx("\\[(\\d+):(\\d+\\.\\d+)\\]");
                int pos = 0;
                QList<qint64> times;
                // 提取所有时间戳
                while ((pos = rx.indexIn(line, pos)) != -1) {
                    qint64 t = rx.cap(1).toInt()*60000 + int(rx.cap(2).toDouble()*1000);
                    times.append(t);
                    pos += rx.matchedLength();
                }
                // 去掉所有时间戳，剩下歌词文本
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
            // 排序并写入lyrics
            lyrics.clear();
            for (auto it = lyricMap.constBegin(); it != lyricMap.constEnd(); ++it) {
                lyrics.append({it.key(), it.value()});
            }
            std::sort(lyrics.begin(), lyrics.end(), [](const LyricLine &a, const LyricLine &b){ return a.time < b.time; });
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
    update(); // 新增：确保进度条和歌词刷新
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
    } else {
        // 单击：暂停/播放
        decoder->togglePause();
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

    // 左上角视频信息标签
    if (!videoInfoLabel.isEmpty()) {
        QFont infoFont("Microsoft YaHei", 10, QFont::Bold); // 修改为微软雅黑
        p.setFont(infoFont);
        p.setPen(Qt::white);
        QRect infoRect = QRect(10, 10, width()/1.5, 24);
        p.setBrush(QColor(0,0,0,128));
        p.setRenderHint(QPainter::Antialiasing, true);
        p.drawRoundedRect(infoRect.adjusted(-4,-2,4,2), 6, 6);
        p.drawText(infoRect, Qt::AlignLeft | Qt::AlignVCenter, videoInfoLabel);
    }

    // 直接绘制叠加层（歌词、进度条）
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
        QFont lyricFont("Microsoft YaHei", 12, QFont::Bold); // 修改为微软雅黑
        p.setFont(lyricFont);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::NoBrush);
        p.save();
        p.setPen(QColor(255, 255, 0, int(255 * opacity)));
        p.drawText(lyricRect, Qt::AlignHCenter | Qt::AlignVCenter, lyrics[currentLyricIndex].text);
        p.restore();
    }
    // 上一行歌词淡出（可选）
    if (lastLyricIndex >= 0 && lastLyricIndex < lyrics.size() && lyricOpacity < 1.0) {
        QFont lyricFont("Microsoft YaHei", 12, QFont::Bold); // 修改为微软雅黑
        p.setFont(lyricFont);
        p.save();
        p.setPen(QColor(255, 255, 0, int(255 * (1.0 - opacity) * 0.7)));
        p.drawText(lyricRect, Qt::AlignHCenter | Qt::AlignVCenter, lyrics[lastLyricIndex].text);
        p.restore();
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
    // ...existing code...
}
