#pragma once
#include <QString>
#include <QVector>
#include <QRegExp>

struct LyricLine {
    qint64 time;
    QString text;
};

class LyricManager {
public:
    LyricManager();
    void loadLyrics(const QString &path);
    void parseLyrics(const QString &lyricText, const QRegExp &rx);
    void updateLyricsIndex(qint64 pts);
    const QVector<LyricLine>& getLyrics() const;
    int getCurrentLyricIndex() const;
    int getLastLyricIndex() const;
    void reset();
private:
    QVector<LyricLine> lyrics;
    int currentLyricIndex;
    int lastLyricIndex;
};
