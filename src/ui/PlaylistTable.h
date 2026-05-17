#ifndef PLAYLISTTABLE_H
#define PLAYLISTTABLE_H

#include <QTableWidget>
#include <QStyledItemDelegate>
#include <QHeaderView>
#include <QPainter>
#include <QElapsedTimer>

class QTimer;

class PlaylistTable : public QTableWidget
{
    Q_OBJECT
public:
    explicit PlaylistTable(QWidget *parent = nullptr);
    void snapshotIdealWidths();
    void enforceRightmostResizeLock();
    int hoveredRow() const { return m_hoveredRow; }
    int playingRow() const { return m_playingRow; }
    int dropIndicatorRow() const { return m_dropIndicatorRow; }
    void setPlayingRow(int row);
    void resetSmoothScroll();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

signals:
    void filesDropped(const QStringList &files, int targetRow);
    void internalRowsMoved(QList<int> sourceRows, int targetRow);
    void deleteRequested();

private:
    void onSectionResized(int logicalIndex, int oldSize, int newSize);
    void onSectionPressed(int logicalIndex);
    void onSectionMoved(int logicalIndex, int oldVisualIndex, int newVisualIndex);
    void applyIdealWidths();
    void tickSmoothScroll();
    int rightmostVisibleLogicalIndex() const;
    int rightmostVisibleVisualIndex() const;

    int m_hoveredRow = -1;
    int m_playingRow = -1;
    int m_dropIndicatorRow = -1;
    bool m_isDragging = false;
    bool m_isAdjusting = false;
    bool m_isEnforcingSectionMove = false;
    bool m_isUpdatingResizeModes = false;
    QPoint m_dragStartPos;
    int m_lockedRightmostLogical = -1;
    QVector<int> m_idealWidths;
    QList<int> m_draggedRows;
    int m_scrollTarget = 0;
    QTimer *m_scrollTimer = nullptr;
    QElapsedTimer m_scrollClock;
};

class RowHoverDelegate : public QStyledItemDelegate
{
public:
    RowHoverDelegate(PlaylistTable *table, QObject *parent = nullptr);
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
private:
    PlaylistTable *m_table;
};

#endif // PLAYLISTTABLE_H
