#include "SubtitleRenderer.h"
#include <QFont>
#include <QFontMetrics>
#include <QTimer>
#include <QColor>
#include <QImage>
#include <QPainterPath>

SubtitleRenderer::SubtitleRenderer(SubtitleManager* manager, ASS_Renderer* assRenderer)
    : subtitleManager(manager), assRenderer(assRenderer) {}

void SubtitleRenderer::setAssRenderer(ASS_Renderer* renderer) {
    assRenderer = renderer;
}

void SubtitleRenderer::drawSrtSubtitles(QPainter &p, const QRect &lyricRect, int overlayFontSize, qint64 currentPts) {
    QString subText;
    const auto &subs = subtitleManager->getSubtitles();
    int curIdx = subtitleManager->getCurrentSubtitleIndex();
    if (curIdx >= 0 && curIdx < subs.size()) {
        subText = subs[curIdx].text;
        lastSubIdx = curIdx;
        lastSubText = subText;
    } else {
        lastSubIdx = -2;
        lastSubText.clear();
        return;
    }
    if (!subText.isEmpty()) {
        // 获取当前字幕的起止时间
        qint64 startTime = subs[curIdx].startTime;
        qint64 endTime = subs[curIdx].endTime;
        // 设置淡入淡出持续时间（毫秒）
        const qint64 fadeDuration = 300;
        int alpha = 255;
        if (currentPts < startTime + fadeDuration) {
            // 淡入
            alpha = static_cast<int>(255.0 * (currentPts - startTime) / fadeDuration);
            if (alpha < 0) alpha = 0;
            if (alpha > 255) alpha = 255;
        } else if (currentPts > endTime - fadeDuration) {
            // 淡出
            alpha = static_cast<int>(255.0 * (endTime - currentPts) / fadeDuration);
            if (alpha < 0) alpha = 0;
            if (alpha > 255) alpha = 255;
        }
        QFont subFont("Microsoft YaHei", overlayFontSize - 2, QFont::Bold);
        p.setFont(subFont);
        QRect textRect = p.fontMetrics().boundingRect(
            lyricRect, Qt::AlignHCenter | Qt::AlignVCenter, subText);
        textRect = textRect.marginsAdded(QMargins(10, 8, 10, 8));
        textRect.moveCenter(lyricRect.center());
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        QColor bgColor(0, 0, 0, alpha * 180 / 255); // 背景透明度同步
        p.setPen(Qt::NoPen);
        p.setBrush(bgColor);
        p.drawRoundedRect(textRect, 12, 12);
        p.restore();
        p.save();
        QColor textColor(255, 255, 255, alpha);
        p.setPen(textColor);
        p.drawText(textRect, Qt::AlignHCenter | Qt::AlignVCenter, subText);
        p.restore();
    }
}

void SubtitleRenderer::drawAssSubtitles(QPainter &p, int w, int h, qint64 currentPts) {
    if (subtitleManager->hasAss() && subtitleManager->getAssTrack() && assRenderer) {
        ass_set_frame_size(assRenderer, w, h);
        int detectChange = 0;
        ASS_Image *img =
            ass_render_frame(assRenderer, subtitleManager->getAssTrack(), currentPts, &detectChange);
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
