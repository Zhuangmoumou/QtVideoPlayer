#pragma once
#include <QPainter>
#include <QRect>
#include <QElapsedTimer>
#include "SubtitleManager.h"
#include <ass/ass.h>

class SubtitleRenderer {
public:
    SubtitleRenderer(SubtitleManager* manager, ASS_Renderer* assRenderer = nullptr);
    void drawSrtSubtitles(QPainter &p, const QRect &lyricRect, int overlayFontSize, qint64 currentPts);
    void drawAssSubtitles(QPainter &p, int width, int height, qint64 currentPts);
    void setAssRenderer(ASS_Renderer* renderer);
private:
    SubtitleManager* subtitleManager;
    ASS_Renderer* assRenderer;
    // 状态变量
    int lastSubIdx = -2;
    qreal fadeOpacity = 1.0;
    QString lastSubText;
    bool fadingOut = false;
    QElapsedTimer subFadeTimer;
};
