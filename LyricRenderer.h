#pragma once
#include <QPainter>
#include <QRect>
#include <QElapsedTimer>
#include "LyricManager.h"

class LyricManager; // 前置声明

class LyricRenderer {
public:
    LyricRenderer(LyricManager* manager);
    void drawLyrics(QPainter &p, const QRect &lyricRect, int overlayFontSize, qreal lyricOpacity, const QElapsedTimer &lyricFadeTimer);

private:
    LyricManager* lyricManager;
};
