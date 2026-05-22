QT += core gui widgets multimedia sql opengl openglwidgets network

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
    src/ui/WinTaskbarButtons.cpp \
    src/ui/MetadataLoaderThread.cpp \
    src/ui/FullscreenPlayer.cpp

win32 {
    LIBS += -lole32 -luuid -lavrt -lshell32 -lcomctl32 -lmingw32

    FFMPEG_DIR = $$PWD/third_party/ffmpeg
    exists($$FFMPEG_DIR/include/libavformat/avformat.h) {
        DEFINES += MUSICPLAYER_HAS_FFMPEG
        INCLUDEPATH += $$FFMPEG_DIR/include
        LIBS += -L$$FFMPEG_DIR/lib -lavformat -lavcodec -lavutil -lswscale -lswresample
    }

    BASS_DIR = $$PWD/third_party/bass
    exists($$BASS_DIR/include/bass.h) {
        DEFINES += MUSICPLAYER_HAS_BASS
        INCLUDEPATH += $$BASS_DIR/include
        LIBS += -L$$BASS_DIR/lib -lbass -lbassflac -lbass_fx -lbassmix

        BASS_BIN = $$shell_path($$BASS_DIR/bin)
        OUT_BIN = $$shell_path($$OUT_PWD)
        OUT_BIN_RELEASE = $$shell_path($$OUT_PWD/release)
        OUT_BIN_DEBUG = $$shell_path($$OUT_PWD/debug)
        QMAKE_POST_LINK += $$quote(cmd /c if exist "$${BASS_BIN}\\*.dll" xcopy /Y /D "$${BASS_BIN}\\*.dll" "$${OUT_BIN}\\" >nul & if exist "$${BASS_BIN}\\*.dll" if exist "$${OUT_BIN_RELEASE}" xcopy /Y /D "$${BASS_BIN}\\*.dll" "$${OUT_BIN_RELEASE}\\" >nul & if exist "$${BASS_BIN}\\*.dll" if exist "$${OUT_BIN_DEBUG}" xcopy /Y /D "$${BASS_BIN}\\*.dll" "$${OUT_BIN_DEBUG}\\" >nul)
    } else {
        message("BASS SDK not found in third_party/bass; building without MUSICPLAYER_HAS_BASS")
    }
}

HEADERS += \
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
    src/ui/WinTaskbarButtons.h \
    src/core/TrackItem.h

RESOURCES += \
    resources/resources.qrc
