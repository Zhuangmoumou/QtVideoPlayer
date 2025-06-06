#include <QApplication>
#include "FileBrowser.h"
#include "VideoPlayer.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication::setApplicationName("RK3326 Player");

    QStringList args = app.arguments();
    if (args.size() > 1) {
        // 带参数启动，直接播放
        QString path = args.at(1);
        VideoPlayer *player = new VideoPlayer;
        player->setWindowState(Qt::WindowFullScreen);
        player->play(path);
        return app.exec();
    } else {
        // 无参数，打开文件管理
        FileBrowser *fb = new FileBrowser("/userdisk/Music");
        fb->setWindowState(Qt::WindowFullScreen);
        QObject::connect(fb, &FileBrowser::fileSelected, [&](const QString &file){
            VideoPlayer *player = new VideoPlayer;
            player->setWindowState(Qt::WindowFullScreen);
            player->play(file);
            fb->close();
        });
        fb->show();
        return app.exec();
    }
}
