#include <QApplication>
#include <QDebug>
#include "VideoPlayer.h"
#include "qapplication.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication::setApplicationName("NewPlayer");

    QStringList args = app.arguments();
    // 支持短参数补全
    bool showHelp = false;
    QString path;
    for (int i = 1; i < args.size(); ++i) {
        QString arg = args.at(i);
        if (arg == "--help" || arg == "-h") {
            showHelp = true;
        } else if (!arg.startsWith("-") && path.isEmpty()) {
            path = arg;
        }
    }

    if (showHelp) {
        // qDebug() << "用法: NewPlayer <视频文件路径>";
        qDebug() << "Usage: NewPlayer <video file path>";
        // qDebug() << "参数:";
        qDebug() << "Options:";
        // qDebug() << "  --help, -h          显示帮助信息";
        qDebug() << "  --help, -h          Show help information";
        return 0;
    }

    if (!path.isEmpty()) {
        // 带参数启动，直接全屏播放
        VideoPlayer *player = new VideoPlayer;
        player->setWindowState(Qt::WindowFullScreen);
        player->play(path);
        return app.exec();
    } else {
        // qDebug() << "未指定视频文件路径。使用 --help 查看用法。";
        qDebug() << "No video file path specified. Use --help to see usage.";
        return 0;
    }
}
