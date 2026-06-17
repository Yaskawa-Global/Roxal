#ifdef ROXAL_ENABLE_QT

// Roxal headers first (signals/slots/emit macro clash), then Qt.
#include "VM.h"
#include "Object.h"
#include "ModuleQtConvert.h"
#include "QtTableModel.h"

#include <QVariant>
#include <QModelIndex>
#include <QList>

#include <stdexcept>
#include <vector>

using namespace roxal;

RoxalTableModel::RoxalTableModel(const Value& rowType, const Value& rows)
    : rowType_(rowType), rows_(rows)
{
    // Columns are installed by buildColumns() after construction (see header).
}

RoxalTableModel::~RoxalTableModel() = default;

ObjList* RoxalTableModel::list() const { return asList(rows_); }

void RoxalTableModel::buildColumns(const Value& columns)
{
    columns_.clear();
    if (!isObjectType(rowType_))
        return;
    ObjObjectType* t = asObjectType(rowType_);
    const auto props = t->orderedPublicProperties();

    // Find a public property by name; reports its const-ness (→ editability).
    auto findProp = [&](const icu::UnicodeString& name, bool& isConstOut) -> bool {
        for (const auto& pv : props)
            if (pv.property->name == name) { isConstOut = pv.property->isConst; return true; }
        return false;
    };
    auto addColumn = [&](const icu::UnicodeString& prop, const QString& header, bool editable) {
        ColumnInfo ci;
        ci.prop     = prop;
        ci.propHash = prop.hashCode();
        ci.propUtf8 = QByteArray::fromStdString(toUTF8StdString(prop));
        ci.header   = header;
        ci.editable = editable;
        columns_.push_back(ci);
    };

    if (columns.isNil()) {
        // Default: every public property is a column, header = the property name.
        for (const auto& pv : props)
            addColumn(pv.property->name,
                      QString::fromStdString(toUTF8StdString(pv.property->name)),
                      !pv.property->isConst);
        return;
    }

    if (!isList(columns))
        throw std::runtime_error("qt.TableModel: columns must be a list (or nil for all properties)");
    ObjList* cl = asList(columns);
    for (int32_t i = 0; i < cl->length(); ++i) {
        Value e = cl->getElement(static_cast<size_t>(i));
        icu::UnicodeString prop;
        QString header;
        if (isString(e)) {
            prop   = asStringObj(e)->s;
            header = QString::fromStdString(toUTF8StdString(prop));
        } else if (isList(e)) {
            ObjList* pair = asList(e);
            if (pair->length() < 2 || !isString(pair->getElement(0)) || !isString(pair->getElement(1)))
                throw std::runtime_error("qt.TableModel: a column must be a property name or a "
                                         "[name, header] pair of strings");
            prop   = asStringObj(pair->getElement(0))->s;
            header = QString::fromStdString(toUTF8StdString(asStringObj(pair->getElement(1))->s));
        } else {
            throw std::runtime_error("qt.TableModel: a column must be a property name (string) or "
                                     "a [name, header] pair");
        }
        bool isConst = false;
        if (!findProp(prop, isConst))
            throw std::runtime_error("qt.TableModel: column '" + toUTF8StdString(prop) +
                                     "' is not a public property of the row type");
        addColumn(prop, header, !isConst);
    }
}

int RoxalTableModel::columnIndexByProp(const QByteArray& prop) const
{
    for (int c = 0; c < columns(); ++c)
        if (columns_[c].propUtf8 == prop) return c;
    return -1;
}

int RoxalTableModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;
    return isList(rows_) ? list()->length() : 0;
}

int RoxalTableModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;
    return static_cast<int>(columns_.size());
}

int RoxalTableModel::count() const { return isList(rows_) ? list()->length() : 0; }

Value RoxalTableModel::rowAt(int i) const
{
    if (!isList(rows_) || i < 0 || i >= list()->length())
        return Value::nilVal();
    return list()->getElement(static_cast<size_t>(i));
}

bool RoxalTableModel::admits(const Value& row) const
{
    if (!isObjectInstance(row) || !isObjectType(rowType_))
        return false;
    return isSubtypeOf(asObjectType(asObjectInstance(row)->instanceType),
                       asObjectType(rowType_));
}

QVariant RoxalTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) return {};
    if (role != Qt::DisplayRole && role != Qt::EditRole) return {};
    const int row = index.row();
    const int col = index.column();
    if (row < 0 || row >= count() || col < 0 || col >= columns()) return {};
    Value rowVal = list()->getElement(static_cast<size_t>(row));
    if (!isObjectInstance(rowVal)) return {};
    // Cheap property read (no VM call) → QVariant; safe during the render pump.
    return toQVariant(asObjectInstance(rowVal)->getProperty(columns_[col].prop));
}

bool RoxalTableModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (!index.isValid() || role != Qt::EditRole) return false;
    const int row = index.row();
    const int col = index.column();
    if (row < 0 || row >= count() || col < 0 || col >= columns()) return false;
    if (!columns_[col].editable) return false;
    Value rowVal = list()->getElement(static_cast<size_t>(row));
    if (!isObjectInstance(rowVal)) return false;
    // Gated write: assign() no-ops when unchanged, so a no-op edit emits no dataChanged.
    const bool changed =
        asObjectInstance(rowVal)->propertySlot(columns_[col].propHash).assign(fromQVariant(value));
    if (changed)
        dataChanged(index, index, QList<int>{ role });
    return true;
}

QVariant RoxalTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole) return {};
    if (orientation == Qt::Horizontal && section >= 0 && section < columns())
        return columns_[section].header;
    if (orientation == Qt::Vertical)
        return section + 1;   // 1-based row numbers
    return {};
}

QHash<int, QByteArray> RoxalTableModel::roleNames() const
{
    // TableView reads the cell value under DisplayRole; the column comes from the index.
    QHash<int, QByteArray> r;
    r.insert(Qt::DisplayRole, QByteArray("display"));
    r.insert(Qt::EditRole, QByteArray("edit"));
    return r;
}

Qt::ItemFlags RoxalTableModel::flags(const QModelIndex& index) const
{
    if (!index.isValid()) return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    const int col = index.column();
    if (col >= 0 && col < columns() && columns_[col].editable)
        f |= Qt::ItemIsEditable;
    return f;
}

void RoxalTableModel::appendRow(const Value& row)
{
    const int n = count();
    if (resetDepth_ > 0) { list()->append(row); return; }
    beginInsertRows(QModelIndex(), n, n);
    list()->append(row);
    endInsertRows();
}

void RoxalTableModel::insertRow(int i, const Value& row)
{
    const int n = count();
    if (i < 0) i = 0;
    if (i > n) i = n;
    if (resetDepth_ > 0) { list()->insertAt(i, row); return; }
    beginInsertRows(QModelIndex(), i, i);
    list()->insertAt(i, row);
    endInsertRows();
}

void RoxalTableModel::removeRow(int i)
{
    if (i < 0 || i >= count()) return;
    if (resetDepth_ > 0) { list()->removeAt(i); return; }
    beginRemoveRows(QModelIndex(), i, i);
    list()->removeAt(i);
    endRemoveRows();
}

void RoxalTableModel::moveRow(int from, int to)
{
    const int n = count();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    if (resetDepth_ > 0) {
        Value v = list()->removeAt(from);
        list()->insertAt(to, v);
        return;
    }
    const int dest = (to > from) ? to + 1 : to;
    if (!beginMoveRows(QModelIndex(), from, from, QModelIndex(), dest)) return;
    Value v = list()->removeAt(from);
    list()->insertAt(to, v);
    endMoveRows();
}

void RoxalTableModel::clearRows()
{
    if (count() == 0) return;
    if (resetDepth_ > 0) { list()->setElements({}); return; }
    beginResetModel();
    list()->setElements({});
    endResetModel();
}

void RoxalTableModel::setRows(const std::vector<Value>& rows)
{
    beginResetModel();
    list()->setElements(rows);
    endResetModel();
}

void RoxalTableModel::beginResetBatch()
{
    if (resetDepth_ == 0) beginResetModel();
    ++resetDepth_;
}

void RoxalTableModel::endResetBatch()
{
    if (resetDepth_ == 0) return;
    if (--resetDepth_ == 0) endResetModel();
}

void RoxalTableModel::rowChanged(int row)
{
    if (row < 0 || row >= count() || columns() == 0) return;
    const QModelIndex l = index(row, 0);
    const QModelIndex r = index(row, columns() - 1);
    dataChanged(l, r);
}

void RoxalTableModel::cellChanged(int row, const QByteArray& columnProp)
{
    if (row < 0 || row >= count()) return;
    const int c = columnIndexByProp(columnProp);
    if (c < 0) return;
    const QModelIndex idx = index(row, c);
    dataChanged(idx, idx);
}

bool RoxalTableModel::setCell(int row, const QByteArray& columnProp, const Value& value)
{
    if (row < 0 || row >= count()) return false;
    const int c = columnIndexByProp(columnProp);
    if (c < 0) return false;
    Value rowVal = list()->getElement(static_cast<size_t>(row));
    if (!isObjectInstance(rowVal)) return false;
    const bool changed = asObjectInstance(rowVal)->propertySlot(columns_[c].propHash).assign(value);
    if (changed) {
        const QModelIndex idx = index(row, c);
        dataChanged(idx, idx);
    }
    return changed;
}

#endif // ROXAL_ENABLE_QT
