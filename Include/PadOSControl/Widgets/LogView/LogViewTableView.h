// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <QColor>
#include <QTableView>
#include <QTimer>

struct LogViewCellPosition
{
    int Row = -1;
    int Column = -1;
    int CharOffset = -1;

    bool IsValid() const { return Row >= 0 && Column >= 0 && CharOffset >= 0; }

    bool operator==(const LogViewCellPosition& rhs) const = default;

    bool operator<(const LogViewCellPosition& rhs) const
    {
        if (Row != rhs.Row) { return Row < rhs.Row; }
        if (Column != rhs.Column) { return Column < rhs.Column; }
        return CharOffset < rhs.CharOffset;
    }
};

struct LogViewTextSelection
{
    LogViewCellPosition Anchor;
    LogViewCellPosition Cursor;

    bool IsValid() const { return Anchor.IsValid() && Cursor.IsValid(); }
    bool IsEmpty() const { return !IsValid() || Anchor == Cursor; }

    LogViewCellPosition Start() const { return (Anchor < Cursor) ? Anchor : Cursor; }
    LogViewCellPosition End() const { return (Anchor < Cursor) ? Cursor : Anchor; }

    bool IntersectsRow(int row) const;
    bool IntersectsCell(int row, int column) const;
    std::pair<int, int> GetCellCharRange(int row, int column, int cellTextLength) const;
};

class LogViewTableView : public QTableView
{
    Q_OBJECT

public:
    explicit LogViewTableView(QWidget* parent = nullptr);

    virtual void setModel(QAbstractItemModel* model) override;

    const LogViewTextSelection& GetTextSelection() const { return m_Selection; }
    void ClearSelection();
    void SelectAll();

    void SetSearchHighlight(const QString& searchText, int currentMatchRow, int currentMatchColumn, int currentMatchCharOffset);
    void SetSearchMetadataEnabled(bool enabled);

    LogViewCellPosition HitTest(const QPoint& viewportPos) const;

protected:
    virtual void paintEvent(QPaintEvent* event) override;
    virtual void resizeEvent(QResizeEvent* event) override;
    virtual void mousePressEvent(QMouseEvent* event) override;
    virtual void mouseMoveEvent(QMouseEvent* event) override;
    virtual void mouseReleaseEvent(QMouseEvent* event) override;
    virtual void mouseDoubleClickEvent(QMouseEvent* event) override;
    virtual void keyPressEvent(QKeyEvent* event) override;

signals:
    void SelectionTextChanged();
    void CopyRequested();

private:
    void DrawSelectionOverlay(QPainter* painter);
    void DrawSimpleCellOverlay(QPainter* painter, const QString& text, const QRect& contentRect, int startChar, int endChar);
    void DrawMessageCellOverlay(QPainter* painter, const QString& text, const QRect& contentRect, int startChar, int endChar);

    void DrawSearchOverlay(QPainter* painter);
    void DrawCellSearchHighlights(QPainter* painter, const QString& text, const QRect& contentRect, const QString& searchText, int currentMatchCharOffset);

    void UpdateSelection(const LogViewCellPosition& cursor);
    void UpdateRowHeights(int first, int last);
    void OnColumnResized(int column, int oldWidth, int newWidth);
    void StartAutoScroll(const QPoint& viewportPos);
    void StopAutoScroll();
    void OnAutoScrollTimeout();

    void SelectWord(const LogViewCellPosition& position);
    void SelectLine(int row);
    QString GetCellText(int row, int column) const;

    void OnResizeDebounceTimeout();
    void OnBatchRecalcTimeout();

    static const QColor     s_SearchMatchColor;
    static const QColor     s_CurrentMatchColor;

    LogViewTextSelection    m_Selection;
    QString                 m_SearchText;
    int                     m_CurrentMatchRow        = -1;
    int                     m_CurrentMatchColumn     = -1;
    int                     m_CurrentMatchCharOffset = -1;
    bool                    m_SearchMetadataEnabled  = false;
    bool                    m_IsDragging = false;
    int                     m_ClickCount = 0;
    QTimer                  m_AutoScrollTimer;
    int                     m_AutoScrollDelta = 0;
    QPoint                  m_LastMousePos;
    QTimer                  m_RowHeightTimer;
    QTimer                  m_BatchRecalcTimer;
    int                     m_BatchRecalcNextRow = 0;
};
