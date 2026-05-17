#pragma once

#include <QObject>
#include <QAbstractNativeEventFilter>

class WinTaskbarButtons : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
public:
    explicit WinTaskbarButtons(QObject *parent = nullptr);
    ~WinTaskbarButtons();

    void attach(quintptr wid);
    void setPlaying(bool playing);

signals:
    void previousClicked();
    void playPauseClicked();
    void nextClicked();

protected:
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    void createButtons();

    void   *m_hwnd     = nullptr;
    void   *m_taskbar  = nullptr;
    bool    m_playing  = false;
    bool    m_attached = false;

    static constexpr unsigned int IDT_PREV      = 1;
    static constexpr unsigned int IDT_PLAYPAUSE = 2;
    static constexpr unsigned int IDT_NEXT      = 3;
};