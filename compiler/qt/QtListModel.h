#pragma once

#ifdef ROXAL_ENABLE_QT

// Roxal headers first — they use identifiers (signals/slots/emit) that Qt defines
// as macros, so <QObject>/<QAbstractListModel> must come after them.
#include "Object.h"   // pulls Value.h

#include <QAbstractListModel>
#include <QHash>
#include <QByteArray>
#include <QPointer>

#include <memory>
#include <vector>

class QSortFilterProxyModel;   // QtCore — owned (not GC-rooted) by QtModelHub for qt.SortFilterModel

namespace roxal {

class RoxalTreeModel;   // QtTreeModel.h — also owned + GC-rooted by QtModelHub

// A moc-free QAbstractListModel backed by a Roxal list of row objects. The row
// type's public properties are the roles (Qt::UserRole+1+i, in declared order):
// data() reads them with getProperty (a cheap map lookup, no VM call — safe during
// the render pump) and setData() writes them through Roxal's already-gated property
// assign(). Change / structural notifications are driven explicitly by the
// qt.ListModel builtins. moc-free: only virtuals are overridden, and notifications
// use the base's protected helpers (beginInsertRows/dataChanged/…), already moc'd
// into QtCore — no Q_OBJECT, no AUTOMOC.
class RoxalListModel : public QAbstractListModel {
public:
    // rowType: an ObjObjectType (object or interface). rows: an ObjList Value the
    // model owns (its elements are the row instances).
    RoxalListModel(const Value& rowType, const Value& rows);
    ~RoxalListModel() override;

    // ---- QAbstractListModel overrides (virtuals only) ----
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    QHash<int, QByteArray> roleNames() const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    // ---- Roxal-driven API (called by the qt.ListModel builtins) ----
    int   count() const;
    Value rowAt(int i) const;                       // the row instance (nil if OOB)
    bool  admits(const Value& row) const;           // isSubtypeOf(row's type, rowType)

    void appendRow(const Value& row);               // auto-bracket (raw inside a reset batch)
    void insertRow(int i, const Value& row);
    void removeRow(int i);
    void moveRow(int from, int to);
    void clearRows();
    void setRows(const std::vector<Value>& rows);   // bulk replace (model reset)

    void beginResetBatch();                         // explicit batch: coalesces into one reset
    void endResetBatch();

    void rowChanged(int row);                        // dataChanged over all roles of a row
    void cellChanged(int row, const QByteArray& roleName);
    bool setCell(int row, const QByteArray& roleName, const Value& value); // gated write + notify

    // ---- GC: the Values this (non-Obj) model holds, for the hub's root provider ----
    const Value& rowsValue() const { return rows_; }
    const Value& typeValue() const { return rowType_; }

private:
    struct RoleInfo {
        int role;
        QByteArray name;
        icu::UnicodeString uname;
        int32_t nameHash;
        bool editable;
    };
    void buildRoles();
    const RoleInfo* roleByInt(int role) const;
    const RoleInfo* roleByName(const QByteArray& name) const;
    ObjList* list() const;

    Value rowType_;   // ObjObjectType (object or interface)
    Value rows_;      // ObjList of row instances (owned by this model)
    std::vector<RoleInfo> roles_;
    QHash<int, QByteArray> roleNames_;
    int resetDepth_ { 0 };
};

// Owns every live RoxalListModel and keeps its Roxal Values (rows + type) alive via
// a GC ExternalRootProvider. Singleton; lifetime bracketed by ModuleQt (init() at
// module load, shutdown() at script-complete teardown), mirroring QtSignalHub.
class QtModelHub {
public:
    static QtModelHub& instance();
    void init();      // register the GC root provider (idempotent)
    void shutdown();  // delete the models + unregister the root provider

    // Create + own a model; returns a non-owning pointer (the hub owns it until
    // shutdown; the qt.ListModel / qt.TreeModel handle holds a QPointer to it).
    RoxalListModel* create(const Value& rowType, const Value& rows);
    RoxalTreeModel* createTree(const Value& rowType, const Value& roots);
    // A QSortFilterProxyModel over `source` (a RoxalListModel); the hub owns it. Holds no
    // Roxal Values (its source is already GC-rooted), so it needs ownership only — not tracing.
    QSortFilterProxyModel* createSortFilter(RoxalListModel* source);

private:
    QtModelHub();
    ~QtModelHub();
    QtModelHub(const QtModelHub&) = delete;
    QtModelHub& operator=(const QtModelHub&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace roxal

#endif // ROXAL_ENABLE_QT
