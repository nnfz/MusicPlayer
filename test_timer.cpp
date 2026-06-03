#include <QCoreApplication>
#include <QTimer>
#include <chrono>
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTimer t;
    t.setInterval(std::chrono::nanoseconds(6944444)); // 144 Hz
    return 0;
}