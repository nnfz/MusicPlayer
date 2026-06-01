#include "MusicPlayer.h"
#include "FullscreenPlayer.h"
#include <QApplication>
#include <QSurfaceFormat>
#include <QFontDatabase>

#ifdef Q_OS_WIN
extern "C" {
    int _argc = 0;
    char** _argv = nullptr;
    int* __imp___argc = &_argc;
    char*** __imp___argv = &_argv;
}
#endif

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    
    // Настройка темной темы
    app.setStyle("Fusion");
    
    QPalette darkPalette;
    darkPalette.setColor(QPalette::Window, QColor(30, 30, 30));
    darkPalette.setColor(QPalette::WindowText, Qt::white);
    darkPalette.setColor(QPalette::Base, QColor(43, 43, 43));
    darkPalette.setColor(QPalette::AlternateBase, QColor(53, 53, 53));
    darkPalette.setColor(QPalette::ToolTipBase, Qt::white);
    darkPalette.setColor(QPalette::ToolTipText, Qt::white);
    darkPalette.setColor(QPalette::Text, Qt::white);
    darkPalette.setColor(QPalette::Button, QColor(53, 53, 53));
    darkPalette.setColor(QPalette::ButtonText, Qt::white);
    darkPalette.setColor(QPalette::BrightText, Qt::red);
    darkPalette.setColor(QPalette::Highlight, QColor(0, 120, 215));
    darkPalette.setColor(QPalette::HighlightedText, Qt::white);
    
    app.setPalette(darkPalette);
    
    int fontId = QFontDatabase::addApplicationFont(":/fonts/Geist-VariableFont.ttf");
    if (fontId != -1) {
        QStringList families = QFontDatabase::applicationFontFamilies(fontId);
        if (!families.isEmpty()) {
            QFont appFont(families.at(0));
            appFont.setPointSize(10);
            appFont.setHintingPreference(QFont::PreferNoHinting);
            app.setFont(appFont);
            FullscreenPlayer::setLyricsFontFamily(families.at(0));
        }
    }

    MusicPlayer player;
    player.show();
    
    return app.exec();
}