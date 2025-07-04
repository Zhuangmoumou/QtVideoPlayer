#include "LyricManager.h"
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <taglib/fileref.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/unsynchronizedlyricsframe.h>
#include <taglib/flacfile.h>
#include <taglib/xiphcomment.h>
#include <algorithm>

LyricManager::LyricManager() : currentLyricIndex(0), lastLyricIndex(-1) {}

void LyricManager::loadLyrics(const QString &path) {
    lyrics.clear();
    currentLyricIndex = 0;
    lastLyricIndex = -1;
    bool embeddedLyricLoaded = false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    QByteArray header = file.peek(16);
    file.close();
    QRegExp rx("\\[(\\d+):(\\d+\\.\\d+)\\]");
    rx.setPatternSyntax(QRegExp::RegExp2);
    if (header.startsWith("ID3") || header.mid(0, 2) == QByteArray::fromHex("FFFB")) {
        TagLib::MPEG::File mp3File(path.toUtf8().constData());
        if (mp3File.isValid() && mp3File.ID3v2Tag()) {
            auto *id3 = mp3File.ID3v2Tag();
            auto usltFrames = id3->frameListMap()["USLT"];
            if (!usltFrames.isEmpty()) {
                for (auto *frame : usltFrames) {
                    auto *uslt = dynamic_cast<TagLib::ID3v2::UnsynchronizedLyricsFrame *>(frame);
                    if (uslt) {
                        QString lyricText = QString::fromStdWString(uslt->text().toWString());
                        parseLyrics(lyricText, rx);
                        embeddedLyricLoaded = !lyrics.isEmpty();
                        if (embeddedLyricLoaded)
                            break;
                    }
                }
            }
        }
    }
    if (header.startsWith("fLaC")) {
        TagLib::FLAC::File flacFile(path.toUtf8().constData());
        if (flacFile.isValid() && flacFile.xiphComment()) {
            auto *comment = flacFile.xiphComment();
            if (comment->contains("LYRICS")) {
                QString lyricText = QString::fromUtf8(comment->fieldListMap()["LYRICS"].toString().toCString(true));
                parseLyrics(lyricText, rx);
                embeddedLyricLoaded = !lyrics.isEmpty();
            }
        }
    }
    if (!embeddedLyricLoaded) {
        QString lrc = QFileInfo(path).absolutePath() + "/" + QFileInfo(path).completeBaseName() + ".lrc";
        if (QFile::exists(lrc)) {
            QFile f(lrc);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream in(&f);
                QString allLyrics = in.readAll();
                parseLyrics(allLyrics, rx);
            }
        }
    }
}

void LyricManager::parseLyrics(const QString &lyricText, const QRegExp &rx) {
    QStringList lines = lyricText.split('\n');
    QHash<qint64, QString> lyricMap;
    for (const QString &line : lines) {
        int pos = 0;
        QList<qint64> times;
        while ((pos = rx.indexIn(line, pos)) != -1) {
            qint64 t = rx.cap(1).toInt() * 60000 + int(rx.cap(2).toDouble() * 1000);
            times.append(t);
            pos += rx.matchedLength();
        }
        QString text = line;
        text = text.remove(rx).trimmed();
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
    std::sort(lyrics.begin(), lyrics.end(), [](const LyricLine &a, const LyricLine &b) { return a.time < b.time; });
}

void LyricManager::updateLyricsIndex(qint64 pts) {
    int idx = currentLyricIndex;
    if (idx + 1 < lyrics.size() && lyrics[idx + 1].time <= pts) {
        while (idx + 1 < lyrics.size() && lyrics[idx + 1].time <= pts) {
            idx++;
        }
    } else if (idx > 0 && lyrics[idx].time > pts) {
        while (idx > 0 && lyrics[idx].time > pts) {
            idx--;
        }
    }
    if (currentLyricIndex != idx) {
        lastLyricIndex = currentLyricIndex;
        currentLyricIndex = idx;
    }
}

const QVector<LyricLine>& LyricManager::getLyrics() const {
    return lyrics;
}

int LyricManager::getCurrentLyricIndex() const {
    return currentLyricIndex;
}

int LyricManager::getLastLyricIndex() const {
    return lastLyricIndex;
}

void LyricManager::reset() {
    lyrics.clear();
    currentLyricIndex = 0;
    lastLyricIndex = -1;
}
