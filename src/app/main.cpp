#include "MusicPlayer.h"
#include <QApplication>
#include <QFontDatabase>

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
    
    QFontDatabase::addApplicationFont(":/fonts/NeueMontreal-Bold.ttf");
    QFontDatabase::addApplicationFont(":/fonts/NeueMontreal-BoldItalic.ttf");
    QFontDatabase::addApplicationFont(":/fonts/NeueMontreal-Italic.ttf");
    QFontDatabase::addApplicationFont(":/fonts/NeueMontreal-Light.ttf");
    QFontDatabase::addApplicationFont(":/fonts/NeueMontreal-LightItalic.ttf");
    QFontDatabase::addApplicationFont(":/fonts/NeueMontreal-Medium.ttf");
    QFontDatabase::addApplicationFont(":/fonts/NeueMontreal-MediumItalic.ttf");
    int fontId = QFontDatabase::addApplicationFont(":/fonts/NeueMontreal-Regular.ttf");
    
    if (fontId != -1) {
        QStringList families = QFontDatabase::applicationFontFamilies(fontId);
        if (!families.isEmpty()) {
            QFont appFont(families.at(0));
            appFont.setStyleStrategy(QFont::PreferAntialias);
            appFont.setPointSize(10);
            app.setFont(appFont);
        }
    }

    MusicPlayer player;
    player.show();
    
    return app.exec();
}