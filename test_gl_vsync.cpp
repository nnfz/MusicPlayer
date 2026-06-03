#include <QApplication>
#include <QMainWindow>
#include <QTimer>
#include <QOpenGLWidget>
#include <QElapsedTimer>
#include <QDebug>
#include <QSurfaceFormat>

class GLWidget : public QOpenGLWidget {
public:
    GLWidget(QWidget *parent = nullptr) : QOpenGLWidget(parent) {
        QSurfaceFormat fmt;
        fmt.setSwapInterval(1);
        setFormat(fmt);
    }
    void paintGL() override {
        glClearColor(0,0,0,1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
};

class TestWindow : public QMainWindow {
public:
    TestWindow() {
        m_gl = new GLWidget(this);
        setCentralWidget(m_gl);
        
        m_timer = new QTimer(this);
        m_timer->setInterval(0);
        connect(m_timer, &QTimer::timeout, this, [this]{
            qint64 now = m_elapsed.elapsed();
            qDebug() << "Tick dt:" << (now - m_last);
            m_last = now;
            m_gl->update();
            
            m_frames++;
            if (m_frames > 100) QApplication::quit();
        });
        m_elapsed.start();
        m_timer->start();
    }
    
    QTimer *m_timer;
    QElapsedTimer m_elapsed;
    qint64 m_last = 0;
    int m_frames = 0;
    GLWidget *m_gl;
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    TestWindow w;
    w.show();
    return app.exec();
}