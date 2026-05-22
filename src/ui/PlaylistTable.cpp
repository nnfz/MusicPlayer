#include "PlaylistTable.h"
#include <QMouseEvent>
#include <QKeyEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QScrollBar>
#include <QWheelEvent>
#include <QTimer>
#include <algorithm>

PlaylistTable::PlaylistTable(QWidget *parent)
    : QTableWidget(parent)
    , m_hoveredRow(-1)
    , m_playingRow(-1)
{
    setAcceptDrops(true);
    setDragEnabled(false);
    setDragDropMode(QAbstractItemView::DropOnly);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setShowGrid(false);
    setMouseTracking(true);
    setDragDropOverwriteMode(false);
    setDropIndicatorShown(false);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    verticalHeader()->setVisible(false);
    horizontalHeader()->setStretchLastSection(false);
    horizontalHeader()->setHighlightSections(true);
    horizontalHeader()->setSectionsClickable(true);
    horizontalHeader()->setSectionsMovable(true);
    horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);

    setStyleSheet(
        "QTableWidget { border: 2px solid #555; border-radius: 5px; background-color: #2b2b2b; color: white; gridline-color: #3d3d3d; }"
        "QTableWidget::item { padding: 5px; border: none; color: white; }"
        "QHeaderView::section { background: transparent; color: white; padding: 8px; border: none; font-weight: bold; }"
        "QHeaderView::section:hover { background-color: #2d2d2d; }");

    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setItemDelegate(new RowHoverDelegate(this, this));

    connect(horizontalHeader(), &QHeaderView::sectionResized,
            this, &PlaylistTable::onSectionResized);
    connect(horizontalHeader(), &QHeaderView::sectionPressed,
            this, &PlaylistTable::onSectionPressed);
    connect(horizontalHeader(), &QHeaderView::sectionMoved,
            this, &PlaylistTable::onSectionMoved);
    connect(horizontalHeader(), &QHeaderView::geometriesChanged,
            this, &PlaylistTable::enforceRightmostResizeLock);

    m_scrollTimer = new QTimer(this);
    m_scrollTimer->setTimerType(Qt::PreciseTimer);
    m_scrollTimer->setInterval(8);
    connect(m_scrollTimer, &QTimer::timeout, this, &PlaylistTable::tickSmoothScroll);
}

void PlaylistTable::snapshotIdealWidths()
{
    m_idealWidths.resize(columnCount());
    for (int i = 0; i < columnCount(); ++i)
        m_idealWidths[i] = columnWidth(i);
}

void PlaylistTable::enforceRightmostResizeLock()
{
    if (m_isUpdatingResizeModes)
        return;

    QHeaderView *header = horizontalHeader();
    if (!header)
        return;

    m_isUpdatingResizeModes = true;

    const int rightmostLogical = rightmostVisibleLogicalIndex();
    for (int logical = 0; logical < columnCount(); ++logical) {
        const QHeaderView::ResizeMode targetMode =
            (logical == rightmostLogical && !isColumnHidden(logical))
                ? QHeaderView::Fixed
                : QHeaderView::Interactive;
        if (header->sectionResizeMode(logical) != targetMode)
            header->setSectionResizeMode(logical, targetMode);
    }
    header->setStretchLastSection(false);

    m_isUpdatingResizeModes = false;
}

void PlaylistTable::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::XButton1 || event->button() == Qt::XButton2)
        return;
    if (event->button() == Qt::LeftButton) {
        m_dragStartPos = event->pos();
        m_isDragging = false;
    }
    QTableWidget::mousePressEvent(event);
}

void PlaylistTable::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void PlaylistTable::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void PlaylistTable::dropEvent(QDropEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        QStringList files;
        for (const QUrl &url : event->mimeData()->urls())
            files << url.toLocalFile();
        int dropRow = rowAt(event->position().toPoint().y());
        if (dropRow < 0)
            dropRow = rowCount();
        emit filesDropped(files, dropRow);
        event->acceptProposedAction();
    }
}

void PlaylistTable::paintEvent(QPaintEvent *event)
{
    QTableWidget::paintEvent(event);

    if (m_dropIndicatorRow >= 0) {
        QPainter painter(viewport());
        int y;
        if (m_dropIndicatorRow < rowCount()) {
            QRect rect = visualItemRect(item(m_dropIndicatorRow, 0));
            y = rect.top();
        } else if (rowCount() > 0) {
            QRect rect = visualItemRect(item(rowCount() - 1, 0));
            y = rect.bottom();
        } else {
            y = 0;
        }
        painter.setPen(QPen(QColor(0, 120, 215), 2));
        painter.drawLine(0, y, viewport()->width(), y);
    }
}

void PlaylistTable::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton) {
        if (!m_isDragging) {
            if ((event->pos() - m_dragStartPos).manhattanLength() >= 10) {
                m_draggedRows.clear();
                for (const QModelIndex &idx : selectionModel()->selectedRows())
                    m_draggedRows.append(idx.row());
                std::sort(m_draggedRows.begin(), m_draggedRows.end());
                if (!m_draggedRows.isEmpty()) {
                    m_isDragging = true;
                    setCursor(Qt::DragMoveCursor);
                }
            }
        }

        if (m_isDragging) {
            int row = rowAt(event->pos().y());
            if (row == -1) row = rowCount();
            if (row != m_dropIndicatorRow) {
                m_dropIndicatorRow = row;
                viewport()->update();
            }
            return;
        }
    }

    int row = rowAt(event->position().toPoint().y());
    if (row != m_hoveredRow) {
        m_hoveredRow = row;
        viewport()->update();
    }
    QTableWidget::mouseMoveEvent(event);
}

void PlaylistTable::leaveEvent(QEvent *event)
{
    m_hoveredRow = -1;
    viewport()->update();
    QTableWidget::leaveEvent(event);
}

void PlaylistTable::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::XButton1 || event->button() == Qt::XButton2)
        return;
    if (m_isDragging && event->button() == Qt::LeftButton) {
        if (!m_draggedRows.isEmpty() && rect().contains(event->pos())) {
            int dropRow = rowAt(event->pos().y());
            if (dropRow == -1) dropRow = rowCount();
            emit internalRowsMoved(m_draggedRows, dropRow);
        }
        m_isDragging = false;
        m_draggedRows.clear();
        m_dropIndicatorRow = -1;
        unsetCursor();
        viewport()->update();
        return;
    }
    QTableWidget::mouseReleaseEvent(event);
}

void PlaylistTable::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Delete) {
        emit deleteRequested();
        return;
    }
    QTableWidget::keyPressEvent(event);
}

void PlaylistTable::setPlayingRow(int row)
{
    if (m_playingRow != row) {
        m_playingRow = row;
        viewport()->update();
    }
}

void PlaylistTable::resetSmoothScroll()
{
    if (m_scrollTimer)
        m_scrollTimer->stop();
    if (verticalScrollBar())
        m_scrollTarget = verticalScrollBar()->value();
    else
        m_scrollTarget = 0;
}

void PlaylistTable::applyIdealWidths()
{
    int vw = viewport()->width();
    if (vw <= 0 || columnCount() == 0) return;
    if (m_idealWidths.size() != columnCount()) return;

    int fixedWidth = isColumnHidden(0) ? 0 : m_idealWidths[0];
    int scalableVw = vw - fixedWidth;

    int idealTotal = 0;
    for (int i = 1; i < columnCount(); ++i) {
        if (!isColumnHidden(i))
            idealTotal += m_idealWidths[i];
    }
    if (idealTotal <= 0 || scalableVw <= 0) return;

    m_isAdjusting = true;

    if (!isColumnHidden(0))
        setColumnWidth(0, m_idealWidths[0]);

    int minSize = horizontalHeader()->minimumSectionSize();
    int assigned = 0;
    int lastVisible = -1;
    for (int i = 1; i < columnCount(); ++i) {
        if (isColumnHidden(i)) continue;
        lastVisible = i;
        int w = qMax(minSize, static_cast<int>(
            static_cast<double>(m_idealWidths[i]) / idealTotal * scalableVw));
        setColumnWidth(i, w);
        assigned += w;
    }
    int diff = scalableVw - assigned;
    if (diff != 0 && lastVisible >= 0)
        setColumnWidth(lastVisible, columnWidth(lastVisible) + diff);

    m_isAdjusting = false;
}

void PlaylistTable::resizeEvent(QResizeEvent *event)
{
    QTableWidget::resizeEvent(event);
    applyIdealWidths();
}

void PlaylistTable::wheelEvent(QWheelEvent *event)
{
    if (!verticalScrollBar()) {
        QTableWidget::wheelEvent(event);
        return;
    }

    if (event->modifiers() & Qt::ControlModifier) {
        QTableWidget::wheelEvent(event);
        return;
    }

    const QPoint angleDelta = event->angleDelta();
    const QPoint pixelDelta = event->pixelDelta();
    if (pixelDelta.isNull() && angleDelta.isNull()) {
        QTableWidget::wheelEvent(event);
        return;
    }

    if (!angleDelta.isNull() && angleDelta.y() == 0 && pixelDelta.isNull()) {
        QTableWidget::wheelEvent(event);
        return;
    }

    int rowH = rowCount() > 0 ? rowHeight(0) : 0;
    if (rowH <= 0)
        rowH = 36;

    int delta = 0;
    if (!pixelDelta.isNull()) {
        delta = pixelDelta.y();
    } else {
        const float steps = angleDelta.y() / 120.0f;
        delta = static_cast<int>(steps * rowH * 2.0f);
    }

    const int maxStep = rowH * 3;
    delta = qBound(-maxStep, delta, maxStep);

    if (delta == 0) {
        event->accept();
        return;
    }

    const int current = verticalScrollBar()->value();
    const int baseTarget = (m_scrollTimer && m_scrollTimer->isActive()) ? m_scrollTarget : current;
    int target = baseTarget - delta;
    target = qBound(verticalScrollBar()->minimum(), target, verticalScrollBar()->maximum());
    m_scrollTarget = target;

    if (m_scrollTimer && !m_scrollTimer->isActive()) {
        m_scrollClock.restart();
        m_scrollTimer->start();
    }

    event->accept();
}

void PlaylistTable::tickSmoothScroll()
{
    if (!verticalScrollBar() || !m_scrollTimer) {
        if (m_scrollTimer)
            m_scrollTimer->stop();
        return;
    }

    const int current = verticalScrollBar()->value();
    const int dist = m_scrollTarget - current;
    if (qAbs(dist) <= 1) {
        verticalScrollBar()->setValue(m_scrollTarget);
        m_scrollTimer->stop();
        return;
    }

    qint64 dt = m_scrollClock.isValid() ? m_scrollClock.restart() : 16;
    dt = qMax<qint64>(1, qMin<qint64>(dt, 40));

    int rowH = rowCount() > 0 ? rowHeight(0) : 0;
    if (rowH <= 0)
        rowH = 36;

    const float alpha = 1.0f - std::exp(-static_cast<float>(dt) / 120.0f);
    int step = static_cast<int>(dist * alpha);
    if (step == 0)
        step = (dist > 0) ? 1 : -1;

    const int maxStep = qMax(4, rowH / 3);
    step = qBound(-maxStep, step, maxStep);

    verticalScrollBar()->setValue(current + step);
}

void PlaylistTable::onSectionResized(int logicalIndex, int oldSize, int newSize)
{
    if (m_isAdjusting) return;
    if (logicalIndex < 0 || logicalIndex >= m_idealWidths.size()) return;

    if (logicalIndex == rightmostVisibleLogicalIndex()) {
        m_isAdjusting = true;
        setColumnWidth(logicalIndex, oldSize);
        m_idealWidths[logicalIndex] = oldSize;
        m_isAdjusting = false;
        return;
    }

    m_idealWidths[logicalIndex] = newSize;

    if (logicalIndex == 0) return;

    m_isAdjusting = true;
    int minSize = horizontalHeader()->minimumSectionSize();
    int vw = viewport()->width();

    int total = 0;
    for (int i = 0; i < columnCount(); ++i) {
        if (!isColumnHidden(i))
            total += columnWidth(i);
    }

    int excess = total - vw;
    if (excess > 0) {
        int vis = horizontalHeader()->visualIndex(logicalIndex);
        int remaining = excess;
        for (int v = vis + 1; v < columnCount() && remaining > 0; ++v) {
            int li = horizontalHeader()->logicalIndex(v);
            if (isColumnHidden(li) || li == 0) continue;
            int canShrink = columnWidth(li) - minSize;
            if (canShrink > 0) {
                int shrink = qMin(remaining, canShrink);
                setColumnWidth(li, columnWidth(li) - shrink);
                m_idealWidths[li] = columnWidth(li);
                remaining -= shrink;
            }
        }
        if (remaining > 0) {
            setColumnWidth(logicalIndex, columnWidth(logicalIndex) - remaining);
            m_idealWidths[logicalIndex] = columnWidth(logicalIndex);
        }
    } else if (excess < 0) {
        int vis = horizontalHeader()->visualIndex(logicalIndex);
        int space = -excess;
        for (int v = vis + 1; v < columnCount(); ++v) {
            int li = horizontalHeader()->logicalIndex(v);
            if (!isColumnHidden(li) && li != 0) {
                setColumnWidth(li, columnWidth(li) + space);
                m_idealWidths[li] = columnWidth(li);
                break;
            }
        }
    }

    m_isAdjusting = false;
}

void PlaylistTable::onSectionPressed(int /*logicalIndex*/)
{
    m_lockedRightmostLogical = rightmostVisibleLogicalIndex();
}

void PlaylistTable::onSectionMoved(int /*logicalIndex*/, int /*oldVisualIndex*/, int /*newVisualIndex*/)
{
    if (m_isEnforcingSectionMove)
        return;

    if (m_lockedRightmostLogical < 0 || isColumnHidden(m_lockedRightmostLogical)) {
        m_lockedRightmostLogical = rightmostVisibleLogicalIndex();
        return;
    }

    QHeaderView *header = horizontalHeader();
    const int lockedVisual = header->visualIndex(m_lockedRightmostLogical);
    const int rightmostVisual = rightmostVisibleVisualIndex();
    if (lockedVisual < 0 || rightmostVisual < 0 || lockedVisual == rightmostVisual)
        return;

    m_isEnforcingSectionMove = true;
    header->moveSection(lockedVisual, rightmostVisual);
    m_isEnforcingSectionMove = false;

    enforceRightmostResizeLock();
}

int PlaylistTable::rightmostVisibleLogicalIndex() const
{
    QHeaderView *header = horizontalHeader();
    if (!header)
        return -1;

    for (int visual = header->count() - 1; visual >= 0; --visual) {
        const int logical = header->logicalIndex(visual);
        if (logical >= 0 && !isColumnHidden(logical))
            return logical;
    }
    return -1;
}

int PlaylistTable::rightmostVisibleVisualIndex() const
{
    QHeaderView *header = horizontalHeader();
    if (!header)
        return -1;

    for (int visual = header->count() - 1; visual >= 0; --visual) {
        const int logical = header->logicalIndex(visual);
        if (logical >= 0 && !isColumnHidden(logical))
            return visual;
    }
    return -1;
}

// ============= RowHoverDelegate =============

RowHoverDelegate::RowHoverDelegate(PlaylistTable *table, QObject *parent)
    : QStyledItemDelegate(parent), m_table(table) {}

void RowHoverDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                             const QModelIndex &index) const
{
    QStyleOptionViewItem opt = option;

    bool isHovered = (index.row() == m_table->hoveredRow());
    bool isSelected = option.state & QStyle::State_Selected;
    bool isPlaying = (index.row() == m_table->playingRow());

    QColor bgColor;
    if (isSelected && isHovered)
        bgColor = QColor(50, 150, 255);
    else if (isSelected)
        bgColor = QColor(0, 120, 215);
    else if (isHovered && isPlaying)
        bgColor = QColor(40, 80, 120);
    else if (isHovered)
        bgColor = QColor(55, 60, 75);
    else if (isPlaying)
        bgColor = QColor(35, 55, 80);
    else if (index.row() % 2 == 1)
        bgColor = QColor(0x32, 0x32, 0x32);

    if (bgColor.isValid())
        painter->fillRect(opt.rect, bgColor);

    if (index.column() == 0) {
        QVariant decoration = index.data(Qt::DecorationRole);
        if (decoration.isValid()) {
            QPixmap pixmap = qvariant_cast<QPixmap>(decoration);
            if (!pixmap.isNull()) {
                int side = qMin(opt.rect.width(), opt.rect.height()) - 4;
                QPixmap scaled = pixmap;
                if (side > 0 && (pixmap.width() > side || pixmap.height() > side)) {
                    const QString key = QStringLiteral("pl-cover-%1-%2")
                        .arg(pixmap.cacheKey())
                        .arg(side);
                    if (!QPixmapCache::find(key, &scaled)) {
                        scaled = pixmap.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                        QPixmapCache::insert(key, scaled);
                    }
                }
                int x = opt.rect.center().x() - scaled.width() / 2;
                int y = opt.rect.center().y() - scaled.height() / 2;
                painter->setRenderHint(QPainter::Antialiasing);
                QPainterPath path;
                path.addRoundedRect(QRectF(x, y, scaled.width(), scaled.height()), 8.0, 8.0);
                painter->save();
                painter->setClipPath(path);
                painter->drawPixmap(x, y, scaled);
                painter->restore();
            }
        }
        return;
    }

    opt.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver | QStyle::State_HasFocus);
    QStyledItemDelegate::paint(painter, opt, index);
}
