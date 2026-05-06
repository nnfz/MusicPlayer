QT += core gui widgets multimedia

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17
CONFIG += no_include_pwd

TARGET = MusicPlayer
TEMPLATE = app

INCLUDEPATH += \
    $$PWD/src/audio \
    $$PWD/src/ui \
    $$PWD/src/core \
    $$PWD/src/app

SOURCES += \
    src/audio/FfmpegDecoderBackend.cpp \
    src/ui/ClickableSlider.cpp \
    src/audio/Equalizer.cpp \
    src/audio/GaplessAudioEngine.cpp \
    src/core/PlaylistManager.cpp \
    src/core/CueParser.cpp \
    src/ui/EqualizerDialog.cpp \
    src/ui/PlaylistTable.cpp \
    src/ui/SettingsDialog.cpp \
    src/core/TrackItem.cpp \
    src/app/main.cpp \
    src/ui/MusicPlayer.cpp \
    src/ui/MetadataLoaderThread.cpp \
    src/ui/FullscreenPlayer.cpp

win32 {
    SOURCES += src/audio/WasapiAudioOutputBackend.cpp
    LIBS += -lole32 -luuid -lavrt

    FFMPEG_DIR = $$PWD/third_party/ffmpeg
    exists($$FFMPEG_DIR/include/libavformat/avformat.h): exists($$FFMPEG_DIR/lib/libavformat.dll.a) {
        DEFINES += MUSICPLAYER_HAS_FFMPEG
        INCLUDEPATH += $$FFMPEG_DIR/include
        LIBS += -L$$FFMPEG_DIR/lib -lavformat -lavcodec -lswresample -lswscale -lavutil

        FFMPEG_BIN = $$shell_path($$FFMPEG_DIR/bin)
        OUT_BIN = $$shell_path($$OUT_PWD)
        OUT_BIN_RELEASE = $$shell_path($$OUT_PWD/release)
        OUT_BIN_DEBUG = $$shell_path($$OUT_PWD/debug)
        QMAKE_POST_LINK += $$quote(cmd /c if exist "$${FFMPEG_BIN}\\*.dll" xcopy /Y /D "$${FFMPEG_BIN}\\*.dll" "$${OUT_BIN}\\" >nul & if exist "$${FFMPEG_BIN}\\*.dll" if exist "$${OUT_BIN_RELEASE}" xcopy /Y /D "$${FFMPEG_BIN}\\*.dll" "$${OUT_BIN_RELEASE}\\" >nul & if exist "$${FFMPEG_BIN}\\*.dll" if exist "$${OUT_BIN_DEBUG}" xcopy /Y /D "$${FFMPEG_BIN}\\*.dll" "$${OUT_BIN_DEBUG}\\" >nul)
    } else {
        message("FFmpeg SDK not found in third_party/ffmpeg; building without MUSICPLAYER_HAS_FFMPEG")
    }
}

HEADERS += \
    src/audio/FfmpegDecoderBackend.h \
    src/audio/AudioDecoderBackend.h \
    src/audio/AudioOutputBackend.h \
    src/audio/WasapiAudioOutputBackend.h \
    src/ui/ClickableSlider.h \
    src/audio/Equalizer.h \
    src/audio/GaplessAudioEngine.h \
    src/core/PlaylistManager.h \
    src/core/CueParser.h \
    src/ui/EqualizerDialog.h \
    src/ui/PlaylistTable.h \
    src/ui/SettingsDialog.h \
    src/ui/MusicPlayer.h \
    src/ui/MetadataLoaderThread.h \
    src/ui/FullscreenPlayer.h \
    src/core/TrackItem.h

RESOURCES += \
    resources/resources.qrc
