#ifdef ROXAL_ENABLE_QT

// Roxal headers first (signals/slots/emit macro clash), then Qt.
#include "VM.h"
#include "Object.h"
#include "SimpleMarkSweepGC.h"
#include "ModuleQtConvert.h"
#include "QtListModel.h"

#include <QVariant>
#include <QModelIndex>
#include <QList>

#include <memory>
#include <vector>

using namespace roxal;

// ============================================================
// RoxalListModel
// ============================================================

RoxalListModel::RoxalListModel(const Value& rowType, const Value& rows)
    : rowType_(rowType), rows_(rows)
{
    buildRoles();
}

RoxalListModel::~RoxalListModel() = default;

ObjList* RoxalListModel::list() const { return asList(rows_); }

void RoxalListModel::buildRoles()
{
    roles_.clear();
    roleNames_.clear();
    if (!isObjectType(rowType_))
        return;
    ObjObjectType* t = asObjectType(rowType_);
    const auto props = t->orderedPublicProperties();
    int i = 0;
    for (const auto& pv : props) {
        RoleInfo ri;
        ri.role     = Qt::UserRole + 1 + i;
        ri.uname    = pv.property->name;
        ri.nameHash = pv.property->name.hashCode();
        ri.name     = QByteArray::fromStdString(toUTF8StdString(pv.property->name));
        ri.editable = !pv.property->isConst;
        roles_.push_back(ri);
        roleNames_.insert(ri.role, ri.name);
        ++i;
    }
}

const RoxalListModel::RoleInfo* RoxalListModel::roleByInt(int role) const
{
    for (const auto& ri : roles_)
        if (ri.role == role) return &ri;
    return nullptr;
}

const RoxalListModel::RoleInfo* RoxalListModel::roleByName(const QByteArray& name) const
{
    for (const auto& ri : roles_)
        if (ri.name == name) return &ri;
    return nullptr;
}

int RoxalListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;   // flat list: children only of the root
    return isList(rows_) ? list()->length() : 0;
}

int RoxalListModel::count() const { return isList(rows_) ? list()->length() : 0; }

Value RoxalListModel::rowAt(int i) const
{
    if (!isList(rows_) || i < 0 || i >= list()->length())
        return Value::nilVal();
    return list()->getElement(static_cast<size_t>(i));
}

bool RoxalListModel::admits(const Value& row) const
{
    if (!isObjectInstance(row) || !isObjectType(rowType_))
        return false;
    return isSubtypeOf(asObjectType(asObjectInstance(row)->instanceType),
                       asObjectType(rowType_));
}

QVariant RoxalListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) return {};
    const int row = index.row();
    if (row < 0 || row >= count()) return {};
    const RoleInfo* ri = roleByInt(role);
    if (!ri) return {};
    Value rowVal = list()->getElement(static_cast<size_t>(row));
    if (!isObjectInstance(rowVal)) return {};
    // Cheap property read (no VM call) → QVariant. Allocation-free on the Roxal heap,
    // so safe to run while the VM is parked in run() and Qt is pumping.
    return toQVariant(asObjectInstance(rowVal)->getProperty(ri->uname));
}

bool RoxalListModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (!index.isValid()) return false;
    const int row = index.row();
    if (row < 0 || row >= count()) return false;
    const RoleInfo* ri = roleByInt(role);
    if (!ri || !ri->editable) return false;
    Value rowVal = list()->getElement(static_cast<size_t>(row));
    if (!isObjectInstance(rowVal)) return false;
    // Gated write: assign() no-ops (returns false) when the value is unchanged, so a
    // UI edit that doesn't actually change anything emits no dataChanged → no loop.
    const bool changed =
        asObjectInstance(rowVal)->propertySlot(ri->nameHash).assign(fromQVariant(value));
    if (changed)
        dataChanged(index, index, QList<int>{ role });
    return true;
}

QHash<int, QByteArray> RoxalListModel::roleNames() const { return roleNames_; }

Qt::ItemFlags RoxalListModel::flags(const QModelIndex& index) const
{
    if (!index.isValid()) return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    // Mark editable if any role is writable; setData enforces per-role editability.
    for (const auto& ri : roles_)
        if (ri.editable) { f |= Qt::ItemIsEditable; break; }
    return f;
}

void RoxalListModel::appendRow(const Value& row)
{
    const int n = count();
    if (resetDepth_ > 0) { list()->append(row); return; }
    beginInsertRows(QModelIndex(), n, n);
    list()->append(row);
    endInsertRows();
}

void RoxalListModel::insertRow(int i, const Value& row)
{
    const int n = count();
    if (i < 0) i = 0;
    if (i > n) i = n;
    if (resetDepth_ > 0) { list()->insertAt(i, row); return; }
    beginInsertRows(QModelIndex(), i, i);
    list()->insertAt(i, row);
    endInsertRows();
}

void RoxalListModel::removeRow(int i)
{
    if (i < 0 || i >= count()) return;
    if (resetDepth_ > 0) { list()->removeAt(i); return; }
    beginRemoveRows(QModelIndex(), i, i);
    list()->removeAt(i);
    endRemoveRows();
}

void RoxalListModel::moveRow(int from, int to)
{
    const int n = count();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    if (resetDepth_ > 0) {
        Value v = list()->removeAt(from);
        list()->insertAt(to, v);
        return;
    }
    // Qt destination semantics: moving down, the destination row is to+1.
    const int dest = (to > from) ? to + 1 : to;
    if (!beginMoveRows(QModelIndex(), from, from, QModelIndex(), dest)) return;
    Value v = list()->removeAt(from);
    list()->insertAt(to, v);
    endMoveRows();
}

void RoxalListModel::clearRows()
{
    if (count() == 0) return;
    if (resetDepth_ > 0) { list()->setElements({}); return; }
    beginResetModel();
    list()->setElements({});
    endResetModel();
}

void RoxalListModel::setRows(const std::vector<Value>& rows)
{
    beginResetModel();
    list()->setElements(rows);   // keep the model's own ObjList identity; replace contents
    endResetModel();
}

void RoxalListModel::beginResetBatch()
{
    if (resetDepth_ == 0) beginResetModel();
    ++resetDepth_;
}

void RoxalListModel::endResetBatch()
{
    if (resetDepth_ == 0) return;   // unbalanced end_reset(): ignore
    if (--resetDepth_ == 0) endResetModel();
}

void RoxalListModel::rowChanged(int row)
{
    if (row < 0 || row >= count()) return;
    const QModelIndex idx = index(row);
    dataChanged(idx, idx);   // all roles
}

void RoxalListModel::cellChanged(int row, const QByteArray& roleName)
{
    if (row < 0 || row >= count()) return;
    const QModelIndex idx = index(row);
    if (const RoleInfo* ri = roleByName(roleName))
        dataChanged(idx, idx, QList<int>{ ri->role });
    else
        dataChanged(idx, idx);
}

bool RoxalListModel::setCell(int row, const QByteArray& roleName, const Value& value)
{
    if (row < 0 || row >= count()) return false;
    const RoleInfo* ri = roleByName(roleName);
    if (!ri) return false;
    Value rowVal = list()->getElement(static_cast<size_t>(row));
    if (!isObjectInstance(rowVal)) return false;
    const bool changed = asObjectInstance(rowVal)->propertySlot(ri->nameHash).assign(value);
    if (changed) {
        const QModelIndex idx = index(row);
        dataChanged(idx, idx, QList<int>{ ri->role });
    }
    return changed;
}

// ============================================================
// QtModelHub (owner + GC ExternalRootProvider)
// ============================================================

struct QtModelHub::Impl : SimpleMarkSweepGC::ExternalRootProvider {
    std::vector<std::unique_ptr<RoxalListModel>> models;
    bool rootRegistered { false };

    // Keep each live model's row list (and its elements, transitively) + row type
    // reachable. These Values are held by C++ (a non-Obj QObject), so the mark-sweep
    // collector can't see them without this.
    void visitRoots(ValueVisitor& visitor) override {
        for (auto& m : models) {
            if (!m) continue;
            visitor.visit(m->typeValue());
            visitor.visit(m->rowsValue());
        }
    }
};

QtModelHub::QtModelHub() : impl_(std::make_unique<Impl>()) {}
QtModelHub::~QtModelHub() { shutdown(); }

QtModelHub& QtModelHub::instance()
{
    static QtModelHub hub;
    return hub;
}

void QtModelHub::init()
{
    if (!impl_->rootRegistered) {
        SimpleMarkSweepGC::instance().registerExternalRootProvider(impl_.get());
        impl_->rootRegistered = true;
    }
}

void QtModelHub::shutdown()
{
    impl_->models.clear();   // delete the QAbstractListModels while Qt is still alive
    if (impl_->rootRegistered) {
        SimpleMarkSweepGC::instance().unregisterExternalRootProvider(impl_.get());
        impl_->rootRegistered = false;
    }
}

RoxalListModel* QtModelHub::create(const Value& rowType, const Value& rows)
{
    auto m = std::make_unique<RoxalListModel>(rowType, rows);
    RoxalListModel* raw = m.get();
    impl_->models.push_back(std::move(m));
    return raw;
}

#endif // ROXAL_ENABLE_QT
