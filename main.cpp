#include <QApplication>
#include <QDebug>
#include <QFileInfo>
#include "VideoPlayer.h"
#include "qapplication.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication::setApplicationName("NewPlayer");

    QStringList args = app.arguments();
    bool showHelp = false;
    bool resumePlayback = true; // 默认启用记忆播放
    QString path;
    for (int i = 1; i < args.size(); ++i) {
        QString arg = args.at(i);
        if (arg == "--help" || arg == "-h") {
            showHelp = true;
        } else if (arg == "--no-memory" || arg == "-nm") {
            resumePlayback = false; // 禁用记忆播放
        } else if (!arg.startsWith("-") && path.isEmpty()) {
            path = arg;
        }
    }

    if (showHelp) {
        qDebug() << "Usage: NewPlayer [options] <video file path>";
        qDebug() << "Options:";
        qDebug() << "  --help, -h          Show help information";
        qDebug() << "  --no-memory, -n     Disable resume playback";
        return 0;
    }

    if (!path.isEmpty()) {
        // 检查路径是否为有效文件
        QFileInfo fileInfo(path);
        if (!fileInfo.exists() || !fileInfo.isFile()) {
            qDebug() << "Invalid video file path:" << path;
            return 1;
        }
        // 带参数启动，直接全屏播放
        VideoPlayer *player = new VideoPlayer;
        player->setWindowState(Qt::WindowFullScreen);
        
        // 设置是否启用记忆播放
        player->setResumeEnabled(resumePlayback);
        
        player->play(path);
        return app.exec();
    } else {
        qDebug() << "No video file path specified. Use --help to see usage.";
        return 0;
    }
}