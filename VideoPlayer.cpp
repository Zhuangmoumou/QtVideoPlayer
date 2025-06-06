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

VideoPlayer::VideoPlayer(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_AcceptTouchEvents);
    setWindowFlags(Qt::FramelessWindowHint);
    // 视频渲染区
    videoWidget = new QOpenGLWidget(this);
    videoWidget->setAutoFillBackground(false);

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
    QString lrc = QFileInfo(path).completeBaseName() + ".lrc";
    if (QFile::exists(lrc)) {
        std::cout << "加载歌词：" << lrc.toStdString() << std::endl;
        QFile f(lrc);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            lyrics.clear();
            QTextStream in(&f);
            while (!in.atEnd()) {
                QString line = in.readLine();
                QRegExp rx("\\[(\\d+):(\\d+\\.\\d+)\\](.*)");
                if (rx.indexIn(line) == 0) {
                    qint64 t = rx.cap(1).toInt()*60000 + int(rx.cap(2).toDouble()*1000);
                    lyrics.append({t, rx.cap(3)});
                }
            }
        }
    }
}

void VideoPlayer::onFrame(const QImage &frame)
{
    // 缓存当前帧并请求重绘
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
    // 更新歌词下标
    while (currentLyricIndex+1 < lyrics.size()
           && lyrics[currentLyricIndex+1].time <= pts) {
        currentLyricIndex++;
    }
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
    qint64 dt = pressTimer.elapsed();
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
    videoWidget->setGeometry(rect());
}

void VideoPlayer::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    // 如果有视频帧则绘制视频帧，否则绘制封面
    if (!currentFrame.isNull()) {
        p.drawImage(rect(), currentFrame);
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
        QFont infoFont("Sans", 10, QFont::Bold);
        p.setFont(infoFont);
        p.setPen(Qt::white);
        QRect infoRect = QRect(10, 10, width()/2, 24);
        p.setBrush(QColor(0,0,0,128));
        p.setRenderHint(QPainter::Antialiasing, true);
        p.drawRoundedRect(infoRect.adjusted(-4,-2,4,2), 6, 6);
        p.drawText(infoRect, Qt::AlignLeft | Qt::AlignVCenter, videoInfoLabel);
    }

    // 绘制叠加层
    updateOverlay();
}

void VideoPlayer::updateOverlay()
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::white);
    p.setFont(QFont("Sans", 12));

    // 歌词
    QRect lyricRect = rect().adjusted(0, height()-70, 0, -10);
    if (currentLyricIndex < lyrics.size()) {
        QFont lyricFont("Sans", 16, QFont::Bold);
        p.setFont(lyricFont);
        p.setPen(Qt::yellow);
        p.drawText(lyricRect, Qt::AlignHCenter | Qt::AlignVCenter, lyrics[currentLyricIndex].text);
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

    QRect fillBar = bar.adjusted(1, 1, int((bar.width() - 2) * pct) - bar.width() + 2, -1);
    p.fillRect(QRect(bar.left() + 1, bar.top() + 1, int((bar.width() - 2) * pct), bar.height() - 2), Qt::white);

    p.end();
}
