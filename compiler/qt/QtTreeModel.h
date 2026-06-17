#pragma once

#ifdef ROXAL_ENABLE_QT

// Roxal headers first — they use identifiers (signals/slots/emit) that Qt defines as
// macros, so <QObject>/<QAbstractItemModel> must come after them.
#include "Object.h"   // pulls Value.h

#include <QAbstractItemModel>
#include <QHash>
#include <QByteArray>

#include <unordered_map>
#include <vector>

namespace roxal {

// A moc-free QAbstractItemModel backed by a tree of Roxal node objects — the hierarchical
// complement to RoxalListModel. A node is an ordinary object; its child nodes live in a
// `children` list property (excluded from the roles). The node type's OTHER public
// properties are the roles (Qt::UserRole+1+i, declared order); data()/setData() read/write
// them exactly like the list model. Structure (index/parent/hasChildren) is derived from
// the `children` lists; a `loc_` map (node → parent + row), rebuilt on each structural
// change, answers parent() in O(1). QModelIndex.internalPointer() is the node's stable
// ObjectInstance* (mark-sweep doesn't move objects; the hub keeps them alive). moc-free:
// only virtuals are overridden; notifications use QtCore's already-moc'd base helpers.
class RoxalTreeModel : public QAbstractItemModel {
public:
    // rowType: an ObjObjectType (object or interface). roots: an ObjList Value the model
    // owns; its elements are the top-level node instances.
    RoxalTreeModel(const Value& rowType, const Value& roots);
    ~RoxalTreeModel() override;

    // ---- QAbstractItemModel overrides (virtuals only) ----
    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& index) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    bool hasChildren(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    QHash<int, QByteArray> roleNames() const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    // ---- Roxal-driven API (by node object; nil parent = root level) ----
    bool  admits(const Value& node) const;            // isSubtypeOf(node's type, rowType)
    int   childCount(const Value& parent) const;      // children of parent (roots if nil)
    Value childAt(const Value& parent, int i) const;  // i-th child (nil if OOB)
    Value parentOf(const Value& node) const;          // parent node (nil if root / unknown)

    void appendChild(const Value& parent, const Value& node);
    void insertChild(const Value& parent, int i, const Value& node);
    void removeChild(const Value& parent, int i);
    void moveChild(const Value& parent, int from, int to);
    void clearChildren(const Value& parent);          // clear parent's children (roots if nil)

    void beginResetBatch();                           // coalesce a batch into one reset
    void endResetBatch();

    void nodeChanged(const Value& node);              // dataChanged over all roles of a node
    void cellChanged(const Value& node, const QByteArray& roleName);
    bool setCell(const Value& node, const QByteArray& roleName, const Value& value);

    // ---- GC: the Values this (non-Obj) model holds, for the hub's root provider ----
    const Value& rootsValue() const { return roots_; }
    const Value& typeValue() const  { return rowType_; }

private:
    struct RoleInfo {
        int role;
        QByteArray name;
        icu::UnicodeString uname;
        int32_t nameHash;
        bool editable;
    };
    struct Loc { ObjectInstance* parent; int row; };  // parent == nullptr → a root node

    void buildRoles();                                // roles = public props except `children`
    const RoleInfo* roleByInt(int role) const;
    const RoleInfo* roleByName(const QByteArray& name) const;

    // The children ObjList of `node` (roots_ when node is nullptr); nullptr if the node has
    // no `children` list (a leaf).
    ObjList* childrenOf(ObjectInstance* node) const;
    void rebuildLoc();                                // walk roots_ + children → loc_
    QModelIndex indexForNode(ObjectInstance* node) const;  // invalid for root/unknown
    ObjectInstance* nodeOf(const QModelIndex& idx) const;  // internalPointer → node (root: nullptr)

    Value rowType_;                 // ObjObjectType
    Value roots_;                   // ObjList of top-level node instances (owned by this model)
    icu::UnicodeString childrenName_;  // "children"
    std::vector<RoleInfo> roles_;
    QHash<int, QByteArray> roleNames_;
    std::unordered_map<ObjectInstance*, Loc> loc_;
    int resetDepth_ { 0 };
};

} // namespace roxal

#endif // ROXAL_ENABLE_QT
