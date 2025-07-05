#include "LyricRenderer.h"
#include <QFont>
#include <QFontMetrics>
#include <QTimer>
#include <QElapsedTimer>
#include <QColor>

LyricRenderer::LyricRenderer(LyricManager* manager)
    : lyricManager(manager) {}

void LyricRenderer::drawLyrics(QPainter &p, const QRect &lyricRect, int overlayFontSize, qreal lyricOpacity, const QElapsedTimer &lyricFadeTimer) {
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
