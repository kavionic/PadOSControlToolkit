// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "PadOSControl/Widgets/LogView/LogViewTableView.h"
#include "PadOSControl/Widgets/LogView/LogViewModel.h"
#include <QTextLayout>

const QColor LogViewTableView::s_SearchMatchColor  = QColor(0xFF, 0xFF, 0x88);
const QColor LogViewTableView::s_CurrentMatchColor = QColor(0xFF, 0x99, 0x00);

///////////////////////////////////////////////////////////////////////////////
/// LogViewTextSelection
///////////////////////////////////////////////////////////////////////////////

bool LogViewTextSelection::IntersectsRow(int row) const
{
    if (!IsValid()) { return false; }
    const auto start = Start();
    const auto end = End();
    return row >= start.Row && row <= end.Row;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool LogViewTextSelection::IntersectsCell(int row, int column) const
{
    if (!IsValid()) {
        return false;
    }
    const auto start = Start();
    const auto end = End();

    if (row < start.Row || row > end.Row) { return false; }

    if (start.Row == end.Row)
    {
        return column >= start.Column && column <= end.Column;
    }

    if (row == start.Row) { return column >= start.Column; }
    if (row == end.Row) { return column <= end.Column; }
    return true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

std::pair<int, int> LogViewTextSelection::GetCellCharRange(int row, int column, int cellTextLength) const
{
    if (!IntersectsCell(row, column)) {
        return {0, 0};
    }

    const auto start = Start();
    const auto end = End();

    int startChar = 0;
    int endChar = cellTextLength;

    if (row == start.Row && column == start.Column) {
        startChar = start.CharOffset;
    }
    if (row == start.Row && column < start.Column) {
        return {0, 0};
    }

    if (row == end.Row && column == end.Column) {
        endChar = end.CharOffset;
    }
    if (row == end.Row && column > end.Column) {
        return {0, 0};
    }

    return {startChar, endChar};
}

///////////////////////////////////////////////////////////////////////////////
/// LogViewTableView
///////////////////////////////////////////////////////////////////////////////

LogViewTableView::LogViewTableView(QWidget* parent)
    : QTableView(parent)
{
    viewport()->setCursor(Qt::IBeamCursor);

    m_AutoScrollTimer.setInterval(50);
    connect(&m_AutoScrollTimer, &QTimer::timeout, this, &LogViewTableView::OnAutoScrollTimeout);
    connect(horizontalHeader(), &QHeaderView::sectionResized, this, &LogViewTableView::OnColumnResized);

    m_RowHeightTimer.setSingleShot(true);
    m_RowHeightTimer.setInterval(50);
    connect(&m_RowHeightTimer, &QTimer::timeout, this, &LogViewTableView::OnResizeDebounceTimeout);

    m_BatchRecalcTimer.setInterval(10);
    connect(&m_BatchRecalcTimer, &QTimer::timeout, this, &LogViewTableView::OnBatchRecalcTimeout);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::setModel(QAbstractItemModel* newModel)
{
    QTableView::setModel(newModel);

    if (newModel != nullptr)
    {
        connect(newModel, &QAbstractItemModel::rowsInserted, this,
                [this](const QModelIndex&, int first, int last) { UpdateRowHeights(first, last); });

        connect(newModel, &QAbstractItemModel::modelReset, this, [this]()
        {
            m_BatchRecalcTimer.stop();
            m_RowHeightTimer.start();
        });
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::paintEvent(QPaintEvent* event)
{
    QTableView::paintEvent(event);

    QPainter painter(viewport());

    if (!m_SearchText.isEmpty()) { DrawSearchOverlay(&painter); }
    if (!m_Selection.IsEmpty())  { DrawSelectionOverlay(&painter); }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::SetSearchHighlight(const QString& searchText, int currentMatchRow, int currentMatchColumn, int currentMatchCharOffset)
{
    m_SearchText             = searchText;
    m_CurrentMatchRow        = currentMatchRow;
    m_CurrentMatchColumn     = currentMatchColumn;
    m_CurrentMatchCharOffset = currentMatchCharOffset;
    viewport()->update();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::SetSearchMetadataEnabled(bool enabled)
{
    m_SearchMetadataEnabled = enabled;
    viewport()->update();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::DrawSearchOverlay(QPainter* painter)
{
    const QAbstractItemModel* tableModel = model();
    if (tableModel == nullptr) { return; }

    const QRect viewRect = viewport()->rect();
    const QModelIndex topIndex    = indexAt(QPoint(0, 0));
    const QModelIndex bottomIndex = indexAt(QPoint(0, viewRect.height() - 1));
    const int firstVisible = topIndex.isValid()    ? topIndex.row()    : 0;
    const int lastVisible  = bottomIndex.isValid() ? bottomIndex.row() : tableModel->rowCount() - 1;

    for (int row = firstVisible; row <= lastVisible; ++row)
    {
        for (int col = 0; col < tableModel->columnCount(); ++col)
        {
            if (isColumnHidden(col)) { continue; }
            if (!m_SearchMetadataEnabled && col != static_cast<int>(LogViewModel::Column::Message)) { continue; }

            const QModelIndex cellIndex = tableModel->index(row, col);
            const QRect cellRect = visualRect(cellIndex);
            if (!cellRect.intersects(viewRect)) { continue; }

            const QString text = tableModel->data(cellIndex, Qt::DisplayRole).toString();
            if (!text.contains(m_SearchText, Qt::CaseInsensitive)) { continue; }

            QStyleOptionViewItem opt;
            initViewItemOption(&opt);
            opt.rect  = cellRect;
            opt.index = cellIndex;
            const int textMargin  = style()->pixelMetric(QStyle::PM_FocusFrameHMargin, &opt, this) + 1;
            const QRect contentRect = style()->subElementRect(QStyle::SE_ItemViewItemText, &opt, this)
                                      .adjusted(textMargin, 0, -textMargin, 0);

            const int currentCharOffset = (row == m_CurrentMatchRow && col == m_CurrentMatchColumn)
                                          ? m_CurrentMatchCharOffset : -1;
            DrawCellSearchHighlights(painter, text, contentRect, m_SearchText, currentCharOffset);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::DrawCellSearchHighlights(QPainter* painter, const QString& text, const QRect& contentRect, const QString& searchText, int currentMatchCharOffset)
{
    QString layoutText = text;
    layoutText.replace('\n', QChar::LineSeparator);
    QTextLayout layout(layoutText, font());
    QTextOption textOption;
    textOption.setWrapMode(QTextOption::WordWrap);
    layout.setTextOption(textOption);

    layout.beginLayout();
    qreal y = 0;
    while (true)
    {
        QTextLine line = layout.createLine();
        if (!line.isValid()) { break; }
        line.setLineWidth(contentRect.width());
        line.setPosition(QPointF(0, y));
        y += line.height();
    }
    layout.endLayout();

    int searchPos = 0;
    while (true)
    {
        const int matchStart = text.indexOf(searchText, searchPos, Qt::CaseInsensitive);
        if (matchStart < 0) { break; }
        const int matchEnd = matchStart + static_cast<int>(searchText.length());

        const QColor& highlightColor = (matchStart == currentMatchCharOffset) ? s_CurrentMatchColor : s_SearchMatchColor;
        const QColor foreground = (highlightColor.lightnessF() > 0.5f) ? QColor(Qt::black) : QColor(Qt::white);

        QRegion highlightRegion;
        for (int i = 0; i < layout.lineCount(); ++i)
        {
            const QTextLine line = layout.lineAt(i);
            const int lineStart = line.textStart();
            int lineEnd = lineStart + line.textLength();

            while (lineEnd > lineStart && (text[lineEnd - 1] == '\n' || text[lineEnd - 1] == '\r')) {
                --lineEnd;
            }

            const int overlapStart = qMax(matchStart, lineStart);
            const int overlapEnd   = qMin(matchEnd,   lineEnd);

            if (overlapStart < overlapEnd)
            {
                const qreal x1 = line.cursorToX(overlapStart);
                const qreal x2 = line.cursorToX(overlapEnd);
                const QRectF lineHlRectF(contentRect.left() + qMin(x1, x2),
                                         contentRect.top()  + line.y(),
                                         qAbs(x2 - x1),
                                         line.height());
                const QRect lineHlRect = lineHlRectF.toAlignedRect();
                painter->fillRect(lineHlRect, highlightColor);
                highlightRegion += lineHlRect;
            }
        }

        if (!highlightRegion.isEmpty())
        {
            painter->save();
            painter->setClipRegion(highlightRegion);
            painter->setPen(foreground);
            painter->setFont(font());
            painter->drawText(contentRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);
            painter->restore();
        }

        searchPos = matchEnd;
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::resizeEvent(QResizeEvent* event)
{
    QTableView::resizeEvent(event);
    m_RowHeightTimer.start();
    m_BatchRecalcTimer.stop();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::DrawSelectionOverlay(QPainter* painter)
{
    const QAbstractItemModel* tableModel = model();
    if (tableModel == nullptr) {
        return;
    }

    const auto selStart = m_Selection.Start();
    const auto selEnd = m_Selection.End();
    const QRect viewRect = viewport()->rect();

    // Determine visible row range to avoid iterating all rows in a large selection.
    const QModelIndex topIndex = indexAt(QPoint(0, 0));
    const QModelIndex bottomIndex = indexAt(QPoint(0, viewRect.height() - 1));
    const int firstVisible = topIndex.isValid() ? topIndex.row() : 0;
    const int lastVisible = bottomIndex.isValid() ? bottomIndex.row() : tableModel->rowCount() - 1;

    const int firstRow = qMax(selStart.Row, firstVisible);
    const int lastRow = qMin(selEnd.Row, lastVisible);

    for (int row = firstRow; row <= lastRow; ++row)
    {
        for (int col = 0; col < tableModel->columnCount(); ++col)
        {
            if (isColumnHidden(col)) {
                continue;
            }

            const QModelIndex index = tableModel->index(row, col);
            const QRect cellRect = visualRect(index);
            if (!cellRect.intersects(viewRect)) { continue; }

            const QString text = tableModel->data(index, Qt::DisplayRole).toString();
            const auto [startChar, endChar] = m_Selection.GetCellCharRange(row, col, static_cast<int>(text.length()));

            if (startChar >= endChar) {
                continue;
            }

            QStyleOptionViewItem opt;
            initViewItemOption(&opt);
            opt.rect = cellRect;
            opt.index = index;
            const int textMargin = style()->pixelMetric(QStyle::PM_FocusFrameHMargin, &opt, this) + 1;
            const QRect contentRect = style()->subElementRect(QStyle::SE_ItemViewItemText, &opt, this)
                                      .adjusted(textMargin, 0, -textMargin, 0);

            if (col == static_cast<int>(LogViewModel::Column::Message)) {
                DrawMessageCellOverlay(painter, text, contentRect, startChar, endChar);
            } else {
                DrawSimpleCellOverlay(painter, text, contentRect, startChar, endChar);
            }
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::DrawSimpleCellOverlay(QPainter* painter, const QString& text, const QRect& contentRect, int startChar, int endChar)
{
    const QFontMetrics fm(font());
    const int selStartX = fm.horizontalAdvance(text, startChar);
    const int selEndX = fm.horizontalAdvance(text, endChar);

    const QRect hlRect(contentRect.left() + selStartX, contentRect.top(),
                       selEndX - selStartX, contentRect.height());

    painter->fillRect(hlRect, palette().highlight());

    painter->save();
    painter->setClipRect(hlRect);
    painter->setPen(palette().color(QPalette::HighlightedText));
    painter->setFont(font());
    painter->drawText(contentRect, Qt::AlignLeft | Qt::AlignTop, text);
    painter->restore();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::DrawMessageCellOverlay(QPainter* painter, const QString& text, const QRect& contentRect, int startChar, int endChar)
{
    QString layoutText = text;
    layoutText.replace('\n', QChar::LineSeparator);
    QTextLayout layout(layoutText, font());
    QTextOption textOption;
    textOption.setWrapMode(QTextOption::WordWrap);
    layout.setTextOption(textOption);

    layout.beginLayout();
    qreal y = 0;
    while (true)
    {
        QTextLine line = layout.createLine();
        if (!line.isValid()) { break; }
        line.setLineWidth(contentRect.width());
        line.setPosition(QPointF(0, y));
        y += line.height();
    }
    layout.endLayout();

    QRegion highlightRegion;
    for (int i = 0; i < layout.lineCount(); ++i)
    {
        const QTextLine line = layout.lineAt(i);
        const int lineStart = line.textStart();
        int lineEnd = lineStart + line.textLength();

        while (lineEnd > lineStart && (text[lineEnd - 1] == '\n' || text[lineEnd - 1] == '\r')) {
            --lineEnd;
        }

        const int overlapStart = qMax(startChar, lineStart);
        const int overlapEnd = qMin(endChar, lineEnd);

        if (overlapStart < overlapEnd)
        {
            const qreal x1 = line.cursorToX(overlapStart);
            const qreal x2 = line.cursorToX(overlapEnd);
            const QRectF lineHlRectF(contentRect.left() + qMin(x1, x2),
                                     contentRect.top() + line.y(),
                                     qAbs(x2 - x1),
                                     line.height());
            const QRect lineHlRect = lineHlRectF.toAlignedRect();
            painter->fillRect(lineHlRect, palette().highlight());
            highlightRegion += lineHlRect;
        }
    }

    if (!highlightRegion.isEmpty())
    {
        painter->save();
        painter->setClipRegion(highlightRegion);
        painter->setPen(palette().color(QPalette::HighlightedText));
        painter->setFont(font());
        painter->drawText(contentRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);
        painter->restore();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::ClearSelection()
{
    m_IsDragging = false;
    StopAutoScroll();

    if (!m_Selection.IsEmpty())
    {
        m_Selection = {};
        viewport()->update();
        emit SelectionTextChanged();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::SelectAll()
{
    const QAbstractItemModel* tableModel = model();

    if (tableModel == nullptr) {
        return;
    }

    const int rowCount = tableModel->rowCount();

    if (rowCount == 0) {
        return;
    }

    const int lastColumn = tableModel->columnCount() - 1;
    const QString lastCellText = tableModel->data(tableModel->index(rowCount - 1, lastColumn), Qt::DisplayRole).toString();

    m_Selection.Anchor = {0, 0, 0};
    m_Selection.Cursor = {rowCount - 1, lastColumn, static_cast<int>(lastCellText.length())};

    viewport()->update();
    emit SelectionTextChanged();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

LogViewCellPosition LogViewTableView::HitTest(const QPoint& viewportPos) const
{
    const QModelIndex index = indexAt(viewportPos);

    if (!index.isValid()) {
        return {};
    }

    const QRect cellRect = visualRect(index);
    const QFontMetrics fm(font());
    const QString text = index.data(Qt::DisplayRole).toString();

    QStyleOptionViewItem opt;
    const_cast<LogViewTableView*>(this)->initViewItemOption(&opt);
    opt.rect = cellRect;
    opt.index = index;
    const int textMargin = style()->pixelMetric(QStyle::PM_FocusFrameHMargin, &opt, this) + 1;
    const QRect textRect = style()->subElementRect(QStyle::SE_ItemViewItemText, &opt, this)
                           .adjusted(textMargin, 0, -textMargin, 0);

    const int localX = viewportPos.x() - textRect.left();

    if (static_cast<LogViewModel::Column>(index.column()) == LogViewModel::Column::Message)
    {
        const int localY = viewportPos.y() - textRect.top();

        QString layoutText = text;
        layoutText.replace('\n', QChar::LineSeparator);
        QTextLayout layout(layoutText, font());
        QTextOption textOption;
        textOption.setWrapMode(QTextOption::WordWrap);
        layout.setTextOption(textOption);

        layout.beginLayout();
        qreal y = 0;
        while (true)
        {
            QTextLine line = layout.createLine();

            if (!line.isValid()) {
                break;
            }
            line.setLineWidth(textRect.width());
            line.setPosition(QPointF(0, y));
            y += line.height();
        }
        layout.endLayout();

        for (int i = 0; i < layout.lineCount(); ++i)
        {
            QTextLine line = layout.lineAt(i);
            if (localY >= line.y() && localY < line.y() + line.height())
            {
                int charPos = line.xToCursor(static_cast<qreal>(localX), QTextLine::CursorBetweenCharacters);
                return {index.row(), index.column(), charPos};
            }
        }
        return {index.row(), index.column(), static_cast<int>(text.length())};
    }

    const int localY = viewportPos.y() - textRect.top();
    Q_UNUSED(localY);

    int charOffset = 0;
    for (int i = 0; i < text.length(); ++i)
    {
        const int charMid = fm.horizontalAdvance(text, i) + fm.horizontalAdvance(text[i]) / 2;
        if (localX < charMid) { break; }
        charOffset = i + 1;
    }
    return {index.row(), index.column(), charOffset};
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
    {
        QTableView::mousePressEvent(event);
        return;
    }

    const LogViewCellPosition pos = HitTest(event->pos());
    if (!pos.IsValid())
    {
        ClearSelection();
        return;
    }

    // Clear built-in row selection so it doesn't show alongside our text selection.
    selectionModel()->clearSelection();

    m_Selection.Anchor = pos;
    m_Selection.Cursor = pos;
    m_IsDragging = true;
    m_ClickCount = 1;
    m_LastMousePos = event->pos();

    viewport()->update();
    emit SelectionTextChanged();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_IsDragging)
    {
        QTableView::mouseMoveEvent(event);
        return;
    }

    m_LastMousePos = event->pos();

    LogViewCellPosition pos = HitTest(event->pos());
    if (pos.IsValid())
    {
        UpdateSelection(pos);
    }
    StartAutoScroll(event->pos());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !m_IsDragging)
    {
        QTableView::mouseReleaseEvent(event);
        return;
    }

    m_IsDragging = false;
    StopAutoScroll();

    LogViewCellPosition pos = HitTest(event->pos());
    if (pos.IsValid())
    {
        UpdateSelection(pos);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
    {
        QTableView::mouseDoubleClickEvent(event);
        return;
    }

    const LogViewCellPosition pos = HitTest(event->pos());

    if (!pos.IsValid()) {
        return;
    }

    // Detect triple-click: if we already have a word-selection from a recent double-click.
    if (m_ClickCount >= 2)
    {
        SelectLine(pos.Row);
        m_ClickCount = 3;
        return;
    }

    SelectWord(pos);
    m_ClickCount = 2;
    m_IsDragging = false;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::keyPressEvent(QKeyEvent* event)
{
    if (event->matches(QKeySequence::Copy))
    {
        emit CopyRequested();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::SelectAll))
    {
        SelectAll();
        event->accept();
        return;
    }

    QScrollBar* vbar = verticalScrollBar();
    switch (event->key())
    {
        case Qt::Key_Up:
            vbar->setValue(vbar->value() - 1);
            event->accept();
            return;
        case Qt::Key_Down:
            vbar->setValue(vbar->value() + 1);
            event->accept();
            return;
        case Qt::Key_PageUp:
            vbar->setValue(vbar->value() - vbar->pageStep());
            event->accept();
            return;
        case Qt::Key_PageDown:
            vbar->setValue(vbar->value() + vbar->pageStep());
            event->accept();
            return;
        case Qt::Key_Home:
            vbar->setValue(vbar->minimum());
            event->accept();
            return;
        case Qt::Key_End:
            vbar->setValue(vbar->maximum());
            event->accept();
            return;
        default:
            break;
    }
    QTableView::keyPressEvent(event);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::UpdateSelection(const LogViewCellPosition& cursor)
{
    if (!(m_Selection.Cursor == cursor))
    {
        m_Selection.Cursor = cursor;
        viewport()->update();
        emit SelectionTextChanged();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::UpdateRowHeights(int first, int last)
{
    const QAbstractItemModel* tableModel = model();

    if (tableModel == nullptr) {
        return;
    }

    const int messageCol = static_cast<int>(LogViewModel::Column::Message);

    if (isColumnHidden(messageCol)) {
        return;
    }

    const int colWidth = viewport()->width() - columnViewportPosition(messageCol);

    if (colWidth <= 0) {
        return;
    }

    QStyleOptionViewItem opt;
    initViewItemOption(&opt);
    opt.rect = QRect(0, 0, colWidth, 0);
    const int textMargin = style()->pixelMetric(QStyle::PM_FocusFrameHMargin, &opt, this) + 1;
    const QRect styleTextRect = style()->subElementRect(QStyle::SE_ItemViewItemText, &opt, this)
                                .adjusted(textMargin, 0, -textMargin, 0);
    const int textWidth = styleTextRect.width();
    const QFontMetrics fm(font());

    for (int row = first; row <= last; ++row)
    {
        const QString text = tableModel->data(tableModel->index(row, messageCol), Qt::DisplayRole).toString();
        const QRect textRect = fm.boundingRect(0, 0, textWidth, 0, Qt::AlignLeft | Qt::TextWordWrap, text);
        const int height = qMax(textRect.height() + 4, 20);
        verticalHeader()->resizeSection(row, height);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::OnColumnResized(int /*column*/, int /*oldWidth*/, int /*newWidth*/)
{
    m_RowHeightTimer.start();
    m_BatchRecalcTimer.stop();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::OnResizeDebounceTimeout()
{
    const QAbstractItemModel* tableModel = model();

    if (tableModel == nullptr) {
        return;
    }

    const QRect viewRect = viewport()->rect();
    const QModelIndex topIndex = indexAt(QPoint(0, 0));
    const QModelIndex bottomIndex = indexAt(QPoint(0, viewRect.height() - 1));
    const int firstVisible = topIndex.isValid() ? topIndex.row() : 0;
    const int lastVisible = bottomIndex.isValid() ? bottomIndex.row() : tableModel->rowCount() - 1;

    UpdateRowHeights(firstVisible, lastVisible);

    m_BatchRecalcNextRow = 0;
    m_BatchRecalcTimer.start();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::OnBatchRecalcTimeout()
{
    const QAbstractItemModel* tableModel = model();
    if (tableModel == nullptr)
    {
        m_BatchRecalcTimer.stop();
        return;
    }

    const int rowCount = tableModel->rowCount();
    const int batchSize = 200;
    const int batchEnd = qMin(m_BatchRecalcNextRow + batchSize, rowCount);

    if (m_BatchRecalcNextRow < rowCount) {
        UpdateRowHeights(m_BatchRecalcNextRow, batchEnd - 1);
    }

    m_BatchRecalcNextRow = batchEnd;
    if (m_BatchRecalcNextRow >= rowCount) {
        m_BatchRecalcTimer.stop();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::StartAutoScroll(const QPoint& viewportPos)
{
    const int viewHeight = viewport()->height();
    if (viewportPos.y() < 0)
    {
        m_AutoScrollDelta = -qMax(1, (-viewportPos.y()) / 10);
    }
    else if (viewportPos.y() > viewHeight)
    {
        m_AutoScrollDelta = qMax(1, (viewportPos.y() - viewHeight) / 10);
    }
    else
    {
        StopAutoScroll();
        return;
    }

    if (!m_AutoScrollTimer.isActive()) {
        m_AutoScrollTimer.start();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::StopAutoScroll()
{
    m_AutoScrollTimer.stop();
    m_AutoScrollDelta = 0;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::OnAutoScrollTimeout()
{
    QScrollBar* vbar = verticalScrollBar();
    vbar->setValue(vbar->value() + m_AutoScrollDelta);

    // Re-hit-test at the last known mouse position to update selection.
    LogViewCellPosition pos = HitTest(m_LastMousePos);

    if (pos.IsValid()) {
        UpdateSelection(pos);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::SelectWord(const LogViewCellPosition& position)
{
    const QString text = GetCellText(position.Row, position.Column);

    if (text.isEmpty()) {
        return;
    }

    const int textLength = static_cast<int>(text.length());
    int offset = qMin(position.CharOffset, textLength - 1);

    if (offset < 0) {
        return;
    }

    int start = offset;
    int end = offset;

    auto isWordChar = [](QChar ch) { return ch.isLetterOrNumber() || ch == '_'; };

    if (isWordChar(text[offset]))
    {
        while (start > 0 && isWordChar(text[start - 1])) { --start; }
        while (end < textLength - 1 && isWordChar(text[end + 1])) { ++end; }
        ++end;
    }
    else
    {
        while (start > 0 && !isWordChar(text[start - 1]) && !text[start - 1].isSpace()) { --start; }
        while (end < textLength - 1 && !isWordChar(text[end + 1]) && !text[end + 1].isSpace()) { ++end; }
        ++end;
    }

    m_Selection.Anchor = {position.Row, position.Column, start};
    m_Selection.Cursor = {position.Row, position.Column, end};
    viewport()->update();
    emit SelectionTextChanged();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewTableView::SelectLine(int row)
{
    const QAbstractItemModel* tableModel = model();

    if (tableModel == nullptr) {
        return;
    }

    const int lastColumn = tableModel->columnCount() - 1;
    const QString lastCellText = tableModel->data(tableModel->index(row, lastColumn), Qt::DisplayRole).toString();

    m_Selection.Anchor = {row, 0, 0};
    m_Selection.Cursor = {row, lastColumn, static_cast<int>(lastCellText.length())};
    viewport()->update();
    emit SelectionTextChanged();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString LogViewTableView::GetCellText(int row, int column) const
{
    const QAbstractItemModel* tableModel = model();
    if (tableModel == nullptr) { return {}; }
    return tableModel->data(tableModel->index(row, column), Qt::DisplayRole).toString();
}
