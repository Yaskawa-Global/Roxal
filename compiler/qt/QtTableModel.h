#pragma once

#ifdef ROXAL_ENABLE_QT

// Roxal headers first — they use identifiers (signals/slots/emit) that Qt defines as
// macros, so <QObject>/<QAbstractTableModel> must come after them.
#include "Object.h"   // pulls Value.h

#include <QAbstractTableModel>
#include <QHash>
#include <QByteArray>
#include <QString>

#include <vector>

namespace roxal {

// A moc-free QAbstractTableModel backed by a Roxal list of row objects. Unlike the list
// model (where every public property is a role on a single column), a table model has an
// explicit set of COLUMNS: each column maps to one of the row type's public properties and
// carries a header. data() returns the cell value under the generic "display"/"edit" roles
// (the column is taken from the QModelIndex), so a TableView delegate binds `model.display`
// and a HorizontalHeaderView reads headerData() for the titles. setData() writes a writable
// (non-const) column back through Roxal's gated property assign(). moc-free: only virtuals
// are overridden; notifications use QtCore's already-moc'd helpers.
class RoxalTableModel : public QAbstractTableModel {
public:
    // rowType: an ObjObjectType (object or interface). rows: an ObjList Value the model
    // owns. Columns are set with buildColumns() AFTER construction (it validates and may
    // throw — doing that from the constructor would unwind through a half-built QObject,
    // which the VM can't turn into a catchable Roxal exception).
    RoxalTableModel(const Value& rowType, const Value& rows);
    ~RoxalTableModel() override;

    // Parse + install the columns: nil (= all public properties, in declared order) or an
    // ObjList whose entries are a property-name string or a [name, header] pair. Throws
    // (a catchable Roxal exception) on a column that isn't a public property of the row type.
    void buildColumns(const Value& columns);

    // ---- QAbstractTableModel overrides (virtuals only) ----
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    // ---- Roxal-driven API (called by the qt.TableModel builtins) ----
    int   count() const;                            // number of rows
    int   columns() const { return static_cast<int>(columns_.size()); }
    Value rowAt(int i) const;                        // the row instance (nil if OOB)
    bool  admits(const Value& row) const;            // isSubtypeOf(row's type, rowType)

    void appendRow(const Value& row);                // auto-bracket (raw inside a reset batch)
    void insertRow(int i, const Value& row);
    void removeRow(int i);
    void moveRow(int from, int to);
    void clearRows();
    void setRows(const std::vector<Value>& rows);

    void beginResetBatch();
    void endResetBatch();

    void rowChanged(int row);                         // dataChanged over the whole row
    void cellChanged(int row, const QByteArray& columnProp);
    bool setCell(int row, const QByteArray& columnProp, const Value& value);

    // ---- GC: the Values this (non-Obj) model holds, for the hub's root provider ----
    const Value& rowsValue() const { return rows_; }
    const Value& typeValue() const { return rowType_; }

private:
    struct ColumnInfo {
        icu::UnicodeString prop;   // property name (for getProperty)
        int32_t           propHash;// for propertySlot
        QByteArray        propUtf8;// for matching column identifiers from Roxal
        QString           header;  // header text
        bool              editable;
    };
    int  columnIndexByProp(const QByteArray& prop) const;
    ObjList* list() const;

    Value rowType_;   // ObjObjectType (object or interface)
    Value rows_;      // ObjList of row instances (owned by this model)
    std::vector<ColumnInfo> columns_;
    int resetDepth_ { 0 };
};

} // namespace roxal

#endif // ROXAL_ENABLE_QT
