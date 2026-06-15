# Roxal Qt/QML Integration — Program of Projects

## Context

Roxal (robotics-oriented language; ANTLR4 → AST → bytecode → VM) currently has no GUI
story. The goal is a **`qt` module** (native `ModuleQt` C++ class) whose purpose is building
**QML / QtQuick UIs driven from Roxal**: the UI is authored in QML + inline JavaScript, and
Roxal can look up Items, read/write properties, register methods QML can call, connect Qt
signals↔Roxal slots, optionally declare/emit signals, and implement Qt model providers
(e.g. `QAbstractListModel`) in Roxal.

Qt is huge; we are **not** wrapping Qt broadly. We are building a small number of **bridges**
across the Roxal↔Qt boundary, on top of a foundation that lets the two runtimes coexist.
Roxal exposes most of the extension points needed; the rest are a few small, sanctioned core
hooks (see Core VM touch-points) — integration work, not VM surgery.

This file is the agreed high-level division into broad projects; it defines scope, dependencies,
and the cross-cutting decisions all projects honor. **P0–P3 are all implemented** (this doc has been
reconciled with what was actually built).

## Resolved architectural decisions

1. **Qt version: Qt 6** (Core, Gui, Qml, Quick).
2. **Execution model: the VM drives; it pumps Qt.** We never call Qt's blocking `app.exec()`;
   Qt is serviced cooperatively via `QGuiApplication::processEvents()`. ⇒ **single-threaded on
   the main thread** — VM and all Qt GUI objects share one thread, so callbacks/event-deliveries
   fire directly with no marshalling.
   - **Top-level VM entry: default to the standard non-yielding entry point.** UI apps are
     generally not real-time. `runFor()` is *primarily* for RT Future Controller integration
     and stays an **optional** mode (offer it, don't require it).
   - **Qt pump = a VM-native `HostEventLoop` hook** (added in P0; *not* `RTCallbackManager`, which
     is a fixed-interval timer that can't block-until-event and invokes callbacks with a mutex held).
     When idle (parked in `engine.run()`/sleep) the dispatch loop blocks in
     `processEvents(QEventLoop::WaitForMoreEvents, timeout)` so a Qt event wakes the VM immediately
     (zero added latency); while busy it throttle-pumps `processEvents()` (~1 ms). Installed at module
     load, before any QML is loaded. No Qt `exec()`; single-threaded, so callbacks fire directly.
   - **Lifetime: explicit, no implicit keep-alive.** When the main Roxal script exits, the app
     exits and tears everything down — via a new `onScriptComplete` module hook fired at the end of
     `VM::run()` (NOT at the VM destructor/atexit, where Qt object teardown crashes). An event-driven
     UI must **explicitly block** waiting for event(s) — window(s) closing, or QML `Qt.quit()` — via
     an `engine.run()` that blocks until the last window closes / quit is requested (pumped meanwhile
     by the host-loop hook). A window being open does NOT by itself keep the VM alive.
   - Constraint to document, not build in v1: Roxal **actors** (worker threads) must not touch
     Qt objects directly (cross-thread).
3. **Signal delivery: both styles** (realized in P2). A connected Qt signal can (a) invoke a
   directly-registered Roxal callable (slot-style, `item.onSignal(fn)`) and/or (b) be bridged to a
   Roxal **event** (`when item.signal occurs`). The reverse direction (QML calling Roxal methods)
   was **deferred** during P2 — a well-engineered app communicates UI→logic via signals→slots.
4. **Memory/ownership:** Qt objects are wrapped as **non-owning** handles (`QPointer`, auto-nulls
   on destruction). Roxal GC collecting a wrapper **never** deletes the Qt object. The reverse
   case (Qt holding a Roxal-implemented model) needs a traceable/explicit-teardown strategy to
   avoid a cross-boundary cycle the mark-sweep collector can't see — owned by P3. **Resolved in P3**
   via a `SimpleMarkSweepGC::ExternalRootProvider` (the `QtModelHub`) that strongly traces each live
   model's row list + row type and is cleared at script-complete teardown; the model shim is C++-owned
   (module-side, like the engines) and the rows are plain data that don't point back, so no cycle.
5. **Property access: native syntax** (`btn.text = "Go"`, `x = slider.value`). Qt property names
   are dynamic, so this needs a **core dynamic-property dispatch hook** — no catch-all exists today
   (the `builtinProperties` table is per-named).
6. **Two value converters, not one** (P1): `QVariant ⇄ Value` (meta-object/property boundary) **and**
   `QJSValue ⇄ Value` (QML inline-JS / JS-engine boundary). Cover primitives, list↔array,
   dict↔object, and Roxal `vector`.
7. **Core VM changes are sanctioned** — small, localized additions are in-scope where they earn
   their keep (see Core VM touch-points).

## Terminology guard

- Roxal `signal()` = **dataflow** (continuous values) — unrelated to Qt. Do not conflate.
- Qt signal/slot ↔ Roxal **events** (`emit` / `when … occurs`) and/or plain callbacks.

## Shared extension points (reference)

- Module scaffold: mirror [ModuleMedia.cpp](compiler/ModuleMedia.cpp) / [ModuleNN.cpp](compiler/ModuleNN.cpp);
  base [BuiltinModule.h](compiler/BuiltinModule.h); lazy factory in [VM.cpp:1141-1166](compiler/VM.cpp#L1141-L1166);
  `@builtin` types + `linkMethod` in a `modules/qt.rox`; `ROXAL_ENABLE_QT` flag in
  [CMakeLists.txt:293-334](CMakeLists.txt#L293-L334) (default OFF).
- Standard non-yielding VM entry point [`VM::run()`](compiler/VM.cpp#L1550): the default launch
  path for UI apps; program completes when frames empty + `hasMoreWork()` false. Optional
  RT/stepped mode (RT Future Controller path): `setup()`/`runFor()`/`hasMoreWork()`/
  `isBlocked()`/`blockedUntil()` in [VM.h:205-223](compiler/VM.h#L205-L223).
- Qt pump (P0, implemented): a VM-native `HostEventLoop` hook (`VM::setHostEventLoop`, with
  `waitForEvents`/`pump`) wired into the dispatch loop's idle-wait + busy-pump paths
  ([VM.cpp ~8910-8990](compiler/VM.cpp#L8910-L8990)). The qt module installs a `QtHostLoop` that
  calls `QGuiApplication::processEvents`. (`RTCallbackManager` was evaluated and rejected — see
  decision #2.)
- Native property dispatch: GetProp/SetProp handlers ([VM.cpp:5888-6224](compiler/VM.cpp#L5888-L6224),
  [VM.cpp:6578-6758](compiler/VM.cpp#L6578-L6758)) + per-`ValueType` `builtinProperties` table
  ([VM.h:731-745](compiler/VM.h#L731-L745)) — per-named only; P1 added the dynamic catch-all hook
  (the 3 `Obj` virtuals, see Core VM touch-points), which `ObjQtObject` overrides.
- Call Roxal from C++: [`invokeClosure()`](compiler/VM.cpp#L3603) (run-to-completion),
  [`pushContinuationCall()`](compiler/VM.cpp#L9484) (non-blocking).
- Events: [`scheduleEventHandlers`](compiler/VM.cpp#L1005), dispatch loop
  [VM.cpp:9253-9481](compiler/VM.cpp#L9253-L9481); types `ObjEventType`/`ObjEventInstance`
  [Object.h:1047](compiler/Object.h#L1047).
- Native object wrapping: `ObjForeignPtr` [Object.h:1139](compiler/Object.h#L1139) (opaque
  ptr + cleanup) or a custom `Obj` subclass; GC via `trace()` + virtual dtor.
- Roxal types implementing interfaces: `ObjObjectType.implementedInterfaces`
  [Object.h:1645](compiler/Object.h#L1645); instance/property access
  [ObjectInstance Object.h:1868](compiler/Object.h#L1868); get/set via GetProp/SetProp +
  `__get_`/`__set_` methods.
- GC note (from CLAUDE.md): any new `Value`-holding struct must be traced in
  [SimpleMarkSweepGC.cpp](compiler/SimpleMarkSweepGC.cpp). For `Value`s held by C++-owned
  (non-`Obj`) structures, register a `SimpleMarkSweepGC::ExternalRootProvider` (virtual `visitRoots`)
  instead — P2's `QtSignalHub` uses this to keep connected closures/event types alive; P3's model
  shim will need the same to hold its Roxal model ref.

## Core VM touch-points (sanctioned)

Small, localized core changes the projects may make (user-approved):

- **Qt pump** (P0, done): a small core `HostEventLoop` hook (`VM::setHostEventLoop`,
  `waitForEvents`/`pump`) — generic and Qt-free; the dispatch loop blocks on it when idle and
  throttle-pumps when busy. (Originally scoped as "no core change, via `RTCallbackManager`"; that
  changed — RTCallbackManager can't block-until-event, so this purpose-built hook replaced it.)
- **Script-complete teardown hook** (P0, done): a new `BuiltinModule::onScriptComplete(VM&)` fired
  at the end of `VM::run()`, while the VM/Qt runtime is still alive. Qt objects (windows/engine/app)
  MUST be torn down here, NOT at the VM destructor/atexit — atexit teardown crashes inside Qt (its
  platform plugin + thread-local state are already gone). `VM::run()` completion semantics are
  otherwise unchanged. (Originally scoped as "no keep-alive hook needed".)
- **Dynamic property/method dispatch** (P1, done): three default-`false` virtuals on `Obj` —
  `tryGetDynamicProperty(self, name, out)`, `trySetDynamicProperty(name, value)`,
  `tryInvokeDynamicMethod(name, args, n, out)` — called from the GetProp/SetProp/`invoke()` catch-alls
  before the "no such property/method" error (Qt-free; the qt wrapper `ObjQtObject` overrides them).
  NB: Roxal method calls compile to GetProp→CALL (methods are func-typed members), so
  `tryGetDynamicProperty` returns a *bound callable* for method names; a new `ValueType::QtObject`
  keeps the wrapper from colliding with builtins registered for `ValueType::Object`.
- **Signals → Roxal** (P2, done): **no core changes.** Built entirely module-side on the P1 dispatch
  virtuals (two new `tryGetDynamicProperty` cases), the existing event system / `scheduleEventHandlers`,
  `invokeClosure`, and the GC `ExternalRootProvider` API. The signal relay is **moc-free** (a
  `qt_metacall` override — no `Q_OBJECT`/AUTOMOC), so even the build is unchanged beyond two new files.
- Keep changes minimal and behind `ROXAL_ENABLE_QT` where they're Qt-specific; the default build
  must be unaffected.

---

## P0 — Foundation: module + Qt lifecycle + loop integration  ✅ implemented

**API shape (chosen).** An `Engine` handle type, not module-level functions:
`var e = qt.Engine(); e.load("ui.qml") / e.load_string(s); e.run(); e.quit()`.

**Scope.** `ModuleQt` skeleton; CMake/Qt6 wiring (`find_package(Qt6 …)`, guarded by
`ROXAL_ENABLE_QT`, OFF by default); `modules/qt.rox` declaration; create a
`QGuiApplication` + `QQmlApplicationEngine`; load QML from **file and from string**; show a
window. Stand up the bridge **machinery before any `load()`** (Qt may call into Roxal during load).

**Pump (implemented).** A VM-native `HostEventLoop` hook: idle →
`processEvents(WaitForMoreEvents, timeout)` (zero-latency); busy → throttled `processEvents()`. The
VM stays in control, no Qt `exec()`. Launch via the **non-yielding entry point**
[`VM::run()`](compiler/VM.cpp#L1550); `runFor()` stepping stays an optional RT add-on.

**Blocking & teardown (implemented).** No implicit keep-alive. Teardown is deterministic via the
`onScriptComplete` hook (end of `VM::run()`, while Qt is alive — never at atexit). The explicit
blocking primitive is **`engine.run()`**: it **cooperatively parks the VM** (sets `threadSleep`,
returns to the dispatch loop — it does NOT block in C++, so Roxal events keep dispatching) until the
last window closes or QML `Qt.quit()` is requested, pumped by the host-loop hook. Window-close is
detected per-window via `QQuickWindow::closing` (NOT `QGuiApplication::lastWindowClosed`, which only
fires under Qt's `exec()`); quit via `QQmlApplicationEngine::quit`. Full signal/event bridging is P2.

**Deliverable.** `roxal` loads a `.qml`, a window appears, the script blocks in `engine.run()`, the
GUI stays responsive (host-loop pump), and **closing the window unblocks `engine.run()` and the
program exits with a clean teardown**.

**Depends on:** nothing.

## P1 — Object handles + properties + method calls  ✅ implemented

**Scope.** A `QPointer`-backed **non-owning** Item handle — a dedicated `Obj` subclass
`ObjQtObject` (NOT `ObjForeignPtr`, since it must override the dispatch virtuals), with a `deref()`
null-guard that raises a catchable exception if the underlying QObject was destroyed. Look up items
by **`objectName`**: `engine.find(name)` (whole tree) and `item.find(name)`/`item.find_all(name)`
(subtree — disambiguates duplicate names / avoids rescans), plus `item.valid()`. (QML `id` is not
runtime-findable; `findChild`/`findChildren` match `objectName`.) Read/write QML properties and call
methods (`Q_INVOKABLE` + QML JS functions) via **native syntax** through the dynamic-dispatch hooks,
plus `qt.get/set/call` escape hatches for non-identifier names. Missing property/method/typo →
**catchable Roxal exception** (fatal if uncaught). Contains the **shared value converters**:
**`QVariant ⇄ Value`** and **`QJSValue ⇄ Value`** (the latter IS needed in P1 — a QML function
returning a JS *object* arrives as a `QJSValue`-wrapped `QVariant`). Cover scalars, string, bool,
list↔array/`QVariantList`, dict↔object/`QVariantMap`, Roxal `vector`→`QVariantList`, nesting,
`QObject*`→Item handle; unsupported reads → nil, writes → error.

**Deliverable (met).** From Roxal: find an Item, read/write a property with native syntax (UI
updates), call a QML/`Q_INVOKABLE` function (incl. a JS function) with args and read its return.

**Depends on:** P0. (QVariant⇄Value is the dependency P2/P3 build on.)

## P2 — Qt signals → Roxal (callbacks + events)  ✅ implemented

**Scope (built).** Qt/QML signal → Roxal, both styles. A **moc-free** C++ relay `QObject`
(`QtSignalRelay`, [compiler/QtSignalRelay.{h,cpp}](compiler/QtSignalRelay.h)) overrides
`qt_metacall` (QSignalSpy-style — no `Q_OBJECT`/AUTOMOC) to connect an arbitrary **named** Qt signal
at runtime by index (`QMetaObject::connect(sender, signalIndex, relay, …)`) and read its args via
`QMetaMethod::parameterMetaType`. A per-`(QObject*, signal)` registry (`QtSignalHub`,
[compiler/QtSignalHub.{h,cpp}](compiler/QtSignalHub.h)) owns one relay per signal and, on emit, both
(a) invokes each registered Roxal callback **synchronously** (`invokeClosure`) and (b) schedules a
Roxal **event** (`scheduleEventHandlers`). Args map through the P1 `QVariant→Value` converter.

**Native syntax (chosen this phase).** Mirrors QML's `on…` handlers, via the P1 `ObjQtObject`
dynamic-dispatch virtuals (two new `tryGetDynamicProperty` cases — **no core change**):
- `item.onSignalName(handler)` — connect a callback (`on`+PascalCase → signal name; handler invoked
  synchronously with the signal's args). Returns a connection id.
- `item.signalName` (bare) — a stable per-`(QObject, signal)` **event type** for `when … occurs`;
  payload keyed by `QMetaMethod::parameterNames()` (fallback `arg0…`), plus `target` = the item handle.
- Escape hatches for dynamic / non-identifier names: `qt.connect(item, name, fn)` / `qt.on(item, name)`
  / `qt.disconnect(conn)`.

**Delivery semantics.** Callbacks run **synchronously** when the signal fires during the host-loop
pump (single-threaded, re-entrant-safe; `invokeClosure` runs a nested `execute()`, bracketed by a
save/clear/restore of `threadSleep` so a callback firing while parked in `engine.run()` runs instead
of re-parking — restore is skipped when the handler called `engine.quit()`, tracked via
`ModuleQt::quitRequested()`). Events are **deferred** through the normal event dispatch loop.

**GC.** Relays are *not* Roxal `Obj`s; the closures + event types they hold are kept alive by a
`SimpleMarkSweepGC::ExternalRootProvider` in `QtSignalHub` (`visitRoots` traces every live-sender
entry) — the CLAUDE.md "trace new Value-holding struct" rule, satisfied via the root provider.

**Deferred this phase (see Open design questions):** the inverse **Roxal → QML** direction (QML
calling Roxal-registered methods) — would need AUTOMOC / `QMetaObjectBuilder`, and a well-engineered
app communicates UI→logic via signals→slots. The stretch (declare/emit Qt signals *from*
Roxal-backed objects) is likewise deferred.

**Deliverable (met).** A QML button's `clicked` signal both (a) invokes a Roxal callback
(`btn.onClicked(fn)`) and (b) is observable via `when btn.clicked occurs` — verified headless
(`QT_QPA_PLATFORM=offscreen`, tests `qt_signal_callback/event/args/disconnect/gc`) and via a real
`gui-user` button click that updates the UI through a Roxal handler.

**Depends on:** P1.

## P3 — Roxal-backed QML list model (rows = objects, roles = properties)  ✅ implemented

**Scope (built).** A **moc-free** C++ `QAbstractListModel` shim (`RoxalListModel`,
[compiler/QtListModel.{h,cpp}](compiler/QtListModel.h)) backed by a Roxal **list of row objects**.
Instead of the traditional model overhead (hand-written `roleNames()`, a `data(row, role)` switch,
string-role bookkeeping), **the row type's public properties are the roles** (`Qt::UserRole+1+i`, in
declared order) — derived once from the `ObjObjectType`. So:
- `rowCount` = the owned list's length; `data(row, role)` = `instance.getProperty(name)` →
  `toQVariant` — a cheap map lookup, **no VM call**, allocation-free → safe during the render pump.
- `setData` (UI edit) writes the row's property through Roxal's already-gated `assign()` (a no-op on
  an unchanged value → no echo loop); a `const` property is a read-only role.
- only virtuals are overridden and notifications use the base's protected helpers
  (`beginInsertRows`/`dataChanged`/…), already moc'd into QtCore — **no `Q_OBJECT`, no AUTOMOC.**

**API.** A curated `@builtin type ListModel` ([modules/qt.rox](modules/qt.rox)), backed by a
`ForeignPtr` to the shim (the `Engine` pattern, not an `ObjQtObject`): `qt.ListModel(rowType[, initial])`
(rowType validated as an object type **or interface**; rows admitted via `isSubtypeOf`, so a subclass
or interface implementer is accepted), `append/insert/remove/move/clear` (auto-bracket Qt's begin/end),
`set_rows` + `begin_reset/end_reset` (batch), `row(i)`/`count()`, and `row_changed`/`cell_changed`/
`set` (explicit change notification). `Engine.set_context_property(name, value)` exposes the model
(or any value) to QML; the view binds `ListView { model: name }` and the delegate reads `model.<prop>`.

**Notification: explicit, not auto.** Roxal→UI changes are announced explicitly (`row_changed`/
`cell_changed`/`set`, or the structural mutators); UI→Roxal edits ride `setData`'s gated write. We
deliberately did **not** auto-observe per-property dataflow signals (that would be O(rows × properties)
subscriptions and pull the dataflow-signal subsystem in) — kept to events/explicit, like P0–P2.

**GC (decision #4, resolved).** The shim is C++-owned module-side (a `QtModelHub`, like the engines);
a `SimpleMarkSweepGC::ExternalRootProvider` traces each live model's row list + row type, cleared at
script-complete teardown. Rows are plain data that don't reference the shim → no cross-boundary cycle.

**Zero core VM changes, no AUTOMOC.** Everything is module-side behind `ROXAL_ENABLE_QT`, reusing the
P1 converters, `getProperty`/`propertySlot().assign()`, and the GC external-root API.

**Deferred (v1 scope):** computed/`__get_` getter properties as roles (would need a receiver-aware
method-invoke + the P2 `threadSleep` dance + per-cell VM cost); a 2-D `QAbstractTableModel`
(properties-as-columns) — the shim is factored so it can follow without rework.

**Deliverable (met).** A QML `ListView` renders rows from a model implemented entirely in Roxal, and
changes flow both ways — verified headless (`qt_model_basic/qml/edit/struct/gc`) and via a real
`gui-user` `ListView` (append a row from Roxal; toggle a checkbox → the Roxal row updates).

**Depends on:** P1, P2.

## Open design questions (future work, post-P3)

- **Roxal → QML** (QML calling Roxal-registered methods) — deferred from P2. Likely unnecessary if
  apps communicate UI→logic via signals→slots (P2) and logic→UI via P1 properties/methods; revisit
  only if a concrete need appears. Would need AUTOMOC or `QMetaObjectBuilder`.
- **Q_PROPERTY-equivalent in Roxal** — letting a Roxal object expose read/write/**notify** properties
  to QML, so QML can bind to Roxal state. P3 already does this **for model rows** (a row's properties
  are QML-bindable roles with explicit change notification); the open part is a **single** Roxal object
  bound as a QML context object with per-property NOTIFY (not via a list model). Its own phase if needed.
- **Computed (`__get_`) roles + 2-D table model** — the deferred parts of P3's v1 scope: getter-backed
  roles (needs a receiver-aware method-invoke + the P2 `threadSleep` dance) and a `QAbstractTableModel`
  variant (properties-as-columns); the P3 shim is factored to grow into both.
- **Qt property ↔ Roxal dataflow `signal`** — whether a changeable Qt property (e.g. `slider.value`,
  with its NOTIFY signal) should surface as a continuous Roxal `signal()` (dataflow), so Roxal reacts
  to UI changes the dataflow way rather than via discrete events. (Keep the terminology guard: Roxal
  `signal()` = dataflow, distinct from Qt signals.)

---

## Verification (whole program)

- **Per project:** add `.rox` tests under `tests/` + entries in `runtests.py`. GUI tests run
  headless via `QT_QPA_PLATFORM=offscreen`; assert on Roxal-side observable state (property
  reads back the written value, callback fired, model row count) rather than pixels.
- **Build:** `cmake -B build/ -DROXAL_ENABLE_QT=ON -DCMAKE_PREFIX_PATH=/path/to/Qt/<ver>/gcc_64`
  then `cmake --build build/ -j4` (the prefix path points `find_package(Qt6)` at a desktop Qt6); the
  default build (flag OFF) must remain unaffected.
- **Manual smoke:** a small QML app run under the `gui-user` MCP / offscreen to confirm a
  window loads, a button click reaches Roxal, and a Roxal-backed list model renders.
