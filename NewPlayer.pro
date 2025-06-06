QT       += core widgets gui multimedia

CONFIG   += c++11
TEMPLATE = app

SOURCES += main.cpp \
           FileBrowser.cpp \
           VideoPlayer.cpp \
           FFMpegDecoder.cpp

HEADERS += FileBrowser.h \
           VideoPlayer.h \
           FFMpegDecoder.h

QMAKE_CXXFLAGS += -Wno-deprecated-declarations

INCLUDEPATH += /home/lyrecoul/ffmpeg-3.4.8/include/ /home/lyrecoul/TagLib/usr/local/include/ /home/lyrecoul/PenDevelopment/NewPlayer/include/
LIBS += -L/home/lyrecoul/PenDevelopment/lib \
        -lavformat -lavcodec -lavutil -lswscale -lswresample \
        -lavdevice -lz -lasound -lavfilter \
        -lfontconfig -lexpat -lpng16 -lglib-2.0 -lpcre2-8 \
        -ldrm -lmali -lfreetype -lssl -lcrypto -lrockchip_mpp -lvorbisenc \
        -lvorbis -lmp3lame -lavresample -lpcre -logg \
        -ltheoradec -ltheoraenc -lm -ldl -lgbm -lwayland-client \
        -lwayland-server -lffi \
        -L/home/lyrecoul/TagLib/usr/local/lib/ -ltag