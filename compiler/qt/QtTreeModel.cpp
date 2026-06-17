#ifdef ROXAL_ENABLE_QT

// Roxal headers first (signals/slots/emit macro clash), then Qt.
#include "VM.h"
#include "Object.h"
#include "ModuleQtConvert.h"
#include "QtTreeModel.h"

#include <QVariant>
#include <QModelIndex>
#include <QList>

#include <functional>

using namespace roxal;

// ============================================================
// RoxalTreeModel
// ============================================================

RoxalTreeModel::RoxalTreeModel(const Value& rowType, const Value& roots)
    : rowType_(rowType), roots_(roots), childrenName_("children")
{
    buildRoles();
    rebuildLoc();
}

RoxalTreeModel::~RoxalTreeModel() = default;

void RoxalTreeModel::buildRoles()
{
    roles_.clear();
    roleNames_.clear();
    if (!isObjectType(rowType_))
        return;
    ObjObjectType* t = asObjectType(rowType_);
    int i = 0;
    for (const auto& pv : t->orderedPublicProperties()) {
        if (pv.property->name == childrenName_)
            continue;   // `children` is structural, not a display role
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

const RoxalTreeModel::RoleInfo* RoxalTreeModel::roleByInt(int role) const
{
    for (const auto& ri : roles_)
        if (ri.role == role) return &ri;
    return nullptr;
}

const RoxalTreeModel::RoleInfo* RoxalTreeModel::roleByName(const QByteArray& name) const
{
    for (const auto& ri : roles_)
        if (ri.name == name) return &ri;
    return nullptr;
}

ObjList* RoxalTreeModel::childrenOf(ObjectInstance* node) const
{
    if (node == nullptr)
        return isList(roots_) ? asList(roots_) : nullptr;   // root level
    Value kids = node->getProperty(childrenName_);
    return isList(kids) ? asList(kids) : nullptr;            // leaf if no `children` list
}

void RoxalTreeModel::rebuildLoc()
{
    loc_.clear();
    std::function<void(ObjectInstance*)> walk = [&](ObjectInstance* parent) {
        ObjList* kids = childrenOf(parent);
        if (!kids) return;
        for (int r = 0; r < kids->length(); ++r) {
            Value cv = kids->getElement(static_cast<size_t>(r));
            if (!isObjectInstance(cv)) continue;
            ObjectInstance* c = asObjectInstance(cv);
            loc_[c] = Loc{ parent, r };
            walk(c);
        }
    };
    walk(nullptr);
}

ObjectInstance* RoxalTreeModel::nodeOf(const QModelIndex& idx) const
{
    return idx.isValid() ? static_cast<ObjectInstance*>(idx.internalPointer()) : nullptr;
}

QModelIndex RoxalTreeModel::indexForNode(ObjectInstance* node) const
{
    if (!node) return {};
    auto it = loc_.find(node);
    if (it == loc_.end()) return {};
    return createIndex(it->second.row, 0, static_cast<void*>(node));
}

QModelIndex RoxalTreeModel::index(int row, int column, const QModelIndex& parent) const
{
    if (column != 0 || row < 0) return {};
    ObjList* kids = childrenOf(nodeOf(parent));
    if (!kids || row >= kids->length()) return {};
    Value cv = kids->getElement(static_cast<size_t>(row));
    if (!isObjectInstance(cv)) return {};
    return createIndex(row, column, static_cast<void*>(asObjectInstance(cv)));
}

QModelIndex RoxalTreeModel::parent(const QModelIndex& idx) const
{
    ObjectInstance* node = nodeOf(idx);
    if (!node) return {};
    auto it = loc_.find(node);
    if (it == loc_.end() || it->second.parent == nullptr) return {};   // root → no parent
    return indexForNode(it->second.parent);
}

int RoxalTreeModel::rowCount(const QModelIndex& parent) const
{
    ObjList* kids = childrenOf(nodeOf(parent));
    return kids ? kids->length() : 0;
}

int RoxalTreeModel::columnCount(const QModelIndex&) const { return 1; }

bool RoxalTreeModel::hasChildren(const QModelIndex& parent) const
{
    ObjList* kids = childrenOf(nodeOf(parent));
    return kids && kids->length() > 0;
}

QVariant RoxalTreeModel::data(const QModelIndex& idx, int role) const
{
    ObjectInstance* node = nodeOf(idx);
    const RoleInfo* ri = roleByInt(role);
    if (!node || !ri) return {};
    // Cheap property read (no VM call) → safe while the VM is parked in run().
    return toQVariant(node->getProperty(ri->uname));
}

bool RoxalTreeModel::setData(const QModelIndex& idx, const QVariant& value, int role)
{
    ObjectInstance* node = nodeOf(idx);
    const RoleInfo* ri = roleByInt(role);
    if (!node || !ri || !ri->editable) return false;
    // Gated write: assign() no-ops when unchanged (no dataChanged → no feedback loop).
    const bool changed = node->propertySlot(ri->nameHash).assign(fromQVariant(value));
    if (changed)
        dataChanged(idx, idx, QList<int>{ role });
    return true;
}

QHash<int, QByteArray> RoxalTreeModel::roleNames() const { return roleNames_; }

Qt::ItemFlags RoxalTreeModel::flags(const QModelIndex& idx) const
{
    if (!idx.isValid()) return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    for (const auto& ri : roles_)
        if (ri.editable) { f |= Qt::ItemIsEditable; break; }
    return f;
}

// ---- Roxal-driven structural / data API ----

bool RoxalTreeModel::admits(const Value& node) const
{
    if (!isObjectInstance(node) || !isObjectType(rowType_))
        return false;
    return isSubtypeOf(asObjectType(asObjectInstance(node)->instanceType), asObjectType(rowType_));
}

int RoxalTreeModel::childCount(const Value& parent) const
{
    ObjList* kids = childrenOf(isObjectInstance(parent) ? asObjectInstance(parent) : nullptr);
    return kids ? kids->length() : 0;
}

Value RoxalTreeModel::childAt(const Value& parent, int i) const
{
    ObjList* kids = childrenOf(isObjectInstance(parent) ? asObjectInstance(parent) : nullptr);
    if (!kids || i < 0 || i >= kids->length()) return Value::nilVal();
    return kids->getElement(static_cast<size_t>(i));
}

Value RoxalTreeModel::parentOf(const Value& nodeVal) const
{
    if (!isObjectInstance(nodeVal)) return Value::nilVal();
    auto it = loc_.find(asObjectInstance(nodeVal));
    if (it == loc_.end() || it->second.parent == nullptr) return Value::nilVal();
    ObjectInstance* parentNode = it->second.parent;
    auto pit = loc_.find(parentNode);
    if (pit == loc_.end()) return Value::nilVal();
    // Re-fetch the parent's own Value from the grandparent's children (or roots).
    ObjList* gk = childrenOf(pit->second.parent);
    if (!gk || pit->second.row >= gk->length()) return Value::nilVal();
    return gk->getElement(static_cast<size_t>(pit->second.row));
}

void RoxalTreeModel::appendChild(const Value& parent, const Value& node)
{
    ObjectInstance* parentNode = isObjectInstance(parent) ? asObjectInstance(parent) : nullptr;
    ObjList* kids = childrenOf(parentNode);
    if (!kids) return;
    const int n = kids->length();
    if (resetDepth_ > 0) { kids->append(node); rebuildLoc(); return; }
    beginInsertRows(indexForNode(parentNode), n, n);
    kids->append(node);
    rebuildLoc();
    endInsertRows();
}

void RoxalTreeModel::insertChild(const Value& parent, int i, const Value& node)
{
    ObjectInstance* parentNode = isObjectInstance(parent) ? asObjectInstance(parent) : nullptr;
    ObjList* kids = childrenOf(parentNode);
    if (!kids) return;
    const int n = kids->length();
    if (i < 0) i = 0;
    if (i > n) i = n;
    if (resetDepth_ > 0) { kids->insertAt(i, node); rebuildLoc(); return; }
    beginInsertRows(indexForNode(parentNode), i, i);
    kids->insertAt(i, node);
    rebuildLoc();
    endInsertRows();
}

void RoxalTreeModel::removeChild(const Value& parent, int i)
{
    ObjectInstance* parentNode = isObjectInstance(parent) ? asObjectInstance(parent) : nullptr;
    ObjList* kids = childrenOf(parentNode);
    if (!kids || i < 0 || i >= kids->length()) return;
    if (resetDepth_ > 0) { kids->removeAt(i); rebuildLoc(); return; }
    beginRemoveRows(indexForNode(parentNode), i, i);
    kids->removeAt(i);
    rebuildLoc();
    endRemoveRows();
}

void RoxalTreeModel::moveChild(const Value& parent, int from, int to)
{
    ObjectInstance* parentNode = isObjectInstance(parent) ? asObjectInstance(parent) : nullptr;
    ObjList* kids = childrenOf(parentNode);
    if (!kids) return;
    const int n = kids->length();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    if (resetDepth_ > 0) {
        Value v = kids->removeAt(from);
        kids->insertAt(to, v);
        rebuildLoc();
        return;
    }
    const QModelIndex p = indexForNode(parentNode);
    const int dest = (to > from) ? to + 1 : to;   // Qt destination semantics
    if (!beginMoveRows(p, from, from, p, dest)) return;
    Value v = kids->removeAt(from);
    kids->insertAt(to, v);
    rebuildLoc();
    endMoveRows();
}

void RoxalTreeModel::clearChildren(const Value& parent)
{
    ObjectInstance* parentNode = isObjectInstance(parent) ? asObjectInstance(parent) : nullptr;
    ObjList* kids = childrenOf(parentNode);
    if (!kids) return;
    const int n = kids->length();
    if (n == 0) return;
    if (resetDepth_ > 0) { kids->setElements({}); rebuildLoc(); return; }
    beginRemoveRows(indexForNode(parentNode), 0, n - 1);
    kids->setElements({});
    rebuildLoc();
    endRemoveRows();
}

void RoxalTreeModel::beginResetBatch()
{
    if (resetDepth_ == 0) beginResetModel();
    ++resetDepth_;
}

void RoxalTreeModel::endResetBatch()
{
    if (resetDepth_ == 0) return;
    if (--resetDepth_ == 0) { rebuildLoc(); endResetModel(); }
}

void RoxalTreeModel::nodeChanged(const Value& node)
{
    const QModelIndex idx = indexForNode(isObjectInstance(node) ? asObjectInstance(node) : nullptr);
    if (idx.isValid()) dataChanged(idx, idx);   // all roles
}

void RoxalTreeModel::cellChanged(const Value& node, const QByteArray& roleName)
{
    const QModelIndex idx = indexForNode(isObjectInstance(node) ? asObjectInstance(node) : nullptr);
    if (!idx.isValid()) return;
    if (const RoleInfo* ri = roleByName(roleName))
        dataChanged(idx, idx, QList<int>{ ri->role });
    else
        dataChanged(idx, idx);
}

bool RoxalTreeModel::setCell(const Value& node, const QByteArray& roleName, const Value& value)
{
    if (!isObjectInstance(node)) return false;
    const RoleInfo* ri = roleByName(roleName);
    if (!ri) return false;
    const bool changed = asObjectInstance(node)->propertySlot(ri->nameHash).assign(value);
    if (changed) {
        const QModelIndex idx = indexForNode(asObjectInstance(node));
        if (idx.isValid()) dataChanged(idx, idx, QList<int>{ ri->role });
    }
    return changed;
}

#endif // ROXAL_ENABLE_QT
