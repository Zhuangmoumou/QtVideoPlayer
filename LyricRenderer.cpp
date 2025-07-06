#include "LyricRenderer.h"
#include <QFont>
#include <QFontMetrics>
#include <QTimer>
#include <QElapsedTimer>
#include <QColor>

LyricRenderer::LyricRenderer(LyricManager* manager)
    : lyricManager(manager) {}

void LyricRenderer::drawLyrics(QPainter &p, const QRect &lyricRect, int overlayFontSize, qreal lyricOpacity, const QElapsedTimer &lyricFadeTimer) {
    // 结合传入的透明度和计时器时间计算当前歌词透明度
    qreal opacity = lyricOpacity;
    if (lyricFadeTimer.isValid()) {
        qint64 elapsed = lyricFadeTimer.elapsed();
        if (elapsed < 400) {
            // 淡入效果：结合基础透明度和时间因子
            opacity = opacity * qMin(1.0, elapsed / 400.0);
        }
        // 否则保持传入的透明度
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
    // 上一行歌词淡出
    if (lastIdx >= 0 && lastIdx < lyrics.size() && lastIdx != curIdx) {
        QFont lyricFont("Microsoft YaHei", overlayFontSize - 2, QFont::Bold);
        p.setFont(lyricFont);
        QString lyricText = lyrics[lastIdx].text;
        QRect textRect = p.fontMetrics().boundingRect(
            lyricRect, Qt::AlignHCenter | Qt::AlignVCenter, lyricText);
        textRect = textRect.marginsAdded(QMargins(10, 8, 10, 8));
        textRect.moveCenter(lyricRect.center());
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        // 计算淡出透明度：随着当前歌词的淡入，上一行歌词逐渐淡出
        qreal fadeOutOpacity = 0.0;
        if (lyricFadeTimer.isValid()) {
            qint64 elapsed = lyricFadeTimer.elapsed();
            if (elapsed < 600) { // 淡出时间稍长于淡入时间
                fadeOutOpacity = qMax(0.0, 1.0 - elapsed / 600.0);
            }
        }
        
        QColor bgColor(0, 0, 0, int(180 * fadeOutOpacity));
        p.setPen(Qt::NoPen);
        p.setBrush(bgColor);
        p.drawRoundedRect(textRect, 12, 12);
        p.restore();
        p.save();
        QColor textColor(255, 255, 255, int(255 * fadeOutOpacity));
        p.setPen(textColor);
        p.drawText(textRect, Qt::AlignHCenter | Qt::AlignVCenter, lyricText);
        p.restore();
    }
}
