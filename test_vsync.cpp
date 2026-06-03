#include <QApplication>
#include <QMainWindow>
#include <QEvent>
#include <QWindow>
#include <QElapsedTimer>
#include <QDebug>

class TestWindow : public QMainWindow {
public:
    TestWindow() {
        m_timer.start();
    }
    
    void showEvent(QShowEvent *) override {
        windowHandle()->requestUpdate();
    }
    
    bool event(QEvent *e) override {
        if (e->type() == QEvent::UpdateRequest) {
            qint64 now = m_timer.elapsed();
            qDebug() << "UpdateRequest dt:" << (now - m_last);
            m_last = now;
            m_frames++;
            if (m_frames < 200) {
                windowHandle()->requestUpdate();
            } else {
                QApplication::quit();
            }
        }
        return QMainWindow::event(e);
    }
    
    QElapsedTimer m_timer;
    qint64 m_last = 0;
    int m_frames = 0;
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    TestWindow w;
    w.show();
    return app.exec();
}