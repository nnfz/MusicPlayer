#include "WinTaskbarButtons.h"
#include <QApplication>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <QWidget>
#include <QStyle>

static HICON hIconFromQIcon(QStyle::StandardPixmap sp, QWidget *w)
{
    const QIcon icon = w ? w->style()->standardIcon(sp, nullptr, w)
                         : QApplication::style()->standardIcon(sp);
    const QPixmap pm = icon.pixmap(16, 16);
    return pm.toImage().toHICON();
}
#endif

WinTaskbarButtons::WinTaskbarButtons(QObject *parent)
    : QObject(parent)
{
#ifdef Q_OS_WIN
    CoInitialize(nullptr);
    ITaskbarList3 *taskbar = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskbarList3, reinterpret_cast<void **>(&taskbar));
    if (SUCCEEDED(hr)) {
        taskbar->HrInit();
        m_taskbar = taskbar;
    }
#endif
    qApp->installNativeEventFilter(this);
}

WinTaskbarButtons::~WinTaskbarButtons()
{
    qApp->removeNativeEventFilter(this);
#ifdef Q_OS_WIN
    if (m_taskbar) {
        reinterpret_cast<ITaskbarList3 *>(m_taskbar)->Release();
        m_taskbar = nullptr;
    }
#endif
}

void WinTaskbarButtons::attach(quintptr wid)
{
#ifdef Q_OS_WIN
    m_hwnd = reinterpret_cast<void *>(wid);
#else
    Q_UNUSED(wid)
#endif
}

void WinTaskbarButtons::setPlaying(bool playing)
{
    if (m_playing == playing)
        return;
    m_playing = playing;

#ifdef Q_OS_WIN
    if (!m_attached || !m_taskbar || !m_hwnd)
        return;

    HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
    ITaskbarList3 *taskbar = reinterpret_cast<ITaskbarList3 *>(m_taskbar);
    QWidget *w = qobject_cast<QWidget *>(parent());

    HIMAGELIST hImgList = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 3, 0);
    if (w) {
        auto addIcon = [&](QStyle::StandardPixmap sp) {
            HICON ico = hIconFromQIcon(sp, w);
            ImageList_AddIcon(hImgList, ico);
            DestroyIcon(ico);
        };
        addIcon(QStyle::SP_MediaSkipBackward);
        addIcon(m_playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay);
        addIcon(QStyle::SP_MediaSkipForward);
    }
    taskbar->ThumbBarSetImageList(hwnd, hImgList);

    THUMBBUTTON buttons[3] = {};

    buttons[0].dwMask  = THB_BITMAP | THB_TOOLTIP | THB_FLAGS;
    buttons[0].iId     = IDT_PREV;
    buttons[0].iBitmap = 0;
    buttons[0].dwFlags = THBF_ENABLED;
    wcscpy_s(buttons[0].szTip, L"Previous");

    buttons[1].dwMask  = THB_BITMAP | THB_TOOLTIP | THB_FLAGS;
    buttons[1].iId     = IDT_PLAYPAUSE;
    buttons[1].iBitmap = 1;
    buttons[1].dwFlags = THBF_ENABLED;
    wcscpy_s(buttons[1].szTip, m_playing ? L"Pause" : L"Play");

    buttons[2].dwMask  = THB_BITMAP | THB_TOOLTIP | THB_FLAGS;
    buttons[2].iId     = IDT_NEXT;
    buttons[2].iBitmap = 2;
    buttons[2].dwFlags = THBF_ENABLED;
    wcscpy_s(buttons[2].szTip, L"Next");

    taskbar->ThumbBarUpdateButtons(hwnd, 3, buttons);
    ImageList_Destroy(hImgList);
#endif
}

void WinTaskbarButtons::createButtons()
{
#ifdef Q_OS_WIN
    if (!m_taskbar || !m_hwnd)
        return;

    HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
    ITaskbarList3 *taskbar = reinterpret_cast<ITaskbarList3 *>(m_taskbar);
    QWidget *w = qobject_cast<QWidget *>(parent());

    HIMAGELIST hImgList = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 3, 0);
    if (w) {
        auto addIcon = [&](QStyle::StandardPixmap sp) {
            HICON ico = hIconFromQIcon(sp, w);
            ImageList_AddIcon(hImgList, ico);
            DestroyIcon(ico);
        };
        addIcon(QStyle::SP_MediaSkipBackward);
        addIcon(QStyle::SP_MediaPlay);
        addIcon(QStyle::SP_MediaSkipForward);
    }
    taskbar->ThumbBarSetImageList(hwnd, hImgList);

    THUMBBUTTON buttons[3] = {};

    buttons[0].dwMask  = THB_BITMAP | THB_TOOLTIP | THB_FLAGS;
    buttons[0].iId     = IDT_PREV;
    buttons[0].iBitmap = 0;
    buttons[0].dwFlags = THBF_ENABLED;
    wcscpy_s(buttons[0].szTip, L"Previous");

    buttons[1].dwMask  = THB_BITMAP | THB_TOOLTIP | THB_FLAGS;
    buttons[1].iId     = IDT_PLAYPAUSE;
    buttons[1].iBitmap = 1;
    buttons[1].dwFlags = THBF_ENABLED;
    wcscpy_s(buttons[1].szTip, L"Play");

    buttons[2].dwMask  = THB_BITMAP | THB_TOOLTIP | THB_FLAGS;
    buttons[2].iId     = IDT_NEXT;
    buttons[2].iBitmap = 2;
    buttons[2].dwFlags = THBF_ENABLED;
    wcscpy_s(buttons[2].szTip, L"Next");

    taskbar->ThumbBarAddButtons(hwnd, 3, buttons);
    ImageList_Destroy(hImgList);
    m_attached = true;
#endif
}

bool WinTaskbarButtons::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
    Q_UNUSED(result)
#ifdef Q_OS_WIN
    if (eventType != "windows_generic_MSG")
        return false;

    MSG *msg = static_cast<MSG *>(message);
    HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
    if (!hwnd || msg->hwnd != hwnd)
        return false;

    static const UINT s_taskbarCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");

    if (msg->message == s_taskbarCreated) {
        m_attached = false;
        createButtons();
        return false;
    }

    if (msg->message == WM_COMMAND) {
        const UINT id = LOWORD(msg->wParam);
        if (id == IDT_PREV)      { emit previousClicked();  return true; }
        if (id == IDT_PLAYPAUSE) { emit playPauseClicked(); return true; }
        if (id == IDT_NEXT)      { emit nextClicked();      return true; }
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
#endif
    return false;
}