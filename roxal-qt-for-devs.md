# Roxal + Qt/QML — Building UIs from Roxal

The optional **`qt` module** lets you build **QML / QtQuick** user interfaces and drive them from
Roxal. The UI is authored in QML (plus inline JavaScript); Roxal loads it, looks up items, reads and
writes their properties, calls their methods, reacts to their signals, feeds list models, and exposes
plain Roxal objects to QML for two-way data binding.

A few cross-cutting facts to keep in mind:

- **The VM drives; Qt is pumped cooperatively.** Roxal never calls Qt's blocking `app.exec()`. The VM
  services Qt's event loop in its own dispatch loop, so the **single main thread** runs both your
  Roxal script and all Qt event handling. Callbacks fire directly, with no thread marshalling.
- **Single-threaded.** Roxal **actors** (worker threads) must **not** touch Qt objects directly.
- **Explicit lifetime.** A window being open does **not** keep the program alive — you block
  explicitly with `engine.run()`, which returns when the last window closes or QML requests a quit.

> **Terminology heads-up:** Roxal already has a `signal` type (dataflow). Qt *also* has "signals".
> They are **not** the same thing — see [Roxal signals vs Qt signals](#roxal-signals-vs-qt-signals)
> below. In the `qt` module, **Qt signals are surfaced to Roxal as events and callbacks**, never as
> Roxal dataflow `signal()`s.

## Building with Qt enabled

The `qt` module is **off by default**. Configure with `ROXAL_ENABLE_QT=ON` and point CMake at a
desktop Qt 6:

```sh
cmake -B build/ -DROXAL_ENABLE_QT=ON -DCMAKE_PREFIX_PATH=/opt/Qt/6.8.3/gcc_64
cmake --build build/ -j4
```

Run a script the usual way: `./build/roxal myapp.rox`. For headless/CI runs, set
`QT_QPA_PLATFORM=offscreen`.

---

## Hello, world

A QML file describing the window:

```qml
// hello.qml
import QtQuick
import QtQuick.Window

Window {
    objectName: "win"
    visible: true
    width: 320; height: 160
    title: "Hello from Roxal"

    Text {
        anchors.centerIn: parent
        text: "Hello, world!"
        font.pixelSize: 24
    }
}
```

And the Roxal script that loads and shows it:

```roxal
import qt

var engine = qt.Engine()
engine.load('hello.qml')   # the QML root must be a Window
engine.run()               # blocks until the window closes / QML Qt.quit()
print('bye')
```

`engine.run()` parks the script while keeping the GUI responsive; it returns when the last window
closes (or QML calls `Qt.quit()` / Roxal calls `engine.quit()`), after which your script finishes and
everything tears down cleanly.

You can also load QML from a string instead of a file:

```roxal
engine.load_string("import QtQuick\nimport QtQuick.Window\nWindow { visible: true; Text { text: 'hi' } }\n")
```

(Use **single-quoted** Roxal strings for inline QML — double-quoted strings interpolate `{...}`, which
clashes with QML's braces.)

---

## The Engine (lifecycle)

`qt.Engine` loads QML and runs the UI loop. Its methods:

| Method | What it does |
|---|---|
| `qt.Engine()` | Create the engine (creates the `QGuiApplication` on first use). |
| `engine.load(path)` | Load QML from a file. The root object must be a `Window`. |
| `engine.load_string(qml)` | Load QML from an inline string. |
| `engine.run()` | Block (cooperatively) until the last window closes / `Qt.quit()`. |
| `engine.quit()` | Ask `run()` to unblock (e.g. from a callback). |
| `engine.find(name)` | Find an item by `objectName` anywhere in the tree (nil if not found). |
| `engine.root()` | The root object as an item handle. |
| `engine.set_context_property(name, value)` | Expose a value/object/model to QML (see below). |

QML `id`s are **not** findable at runtime; set an **`objectName`** on any item you want to reach from
Roxal:

```qml
Button { objectName: "okButton"; text: "OK" }
```

```roxal
var btn = engine.find("okButton")
```

---

## Items & properties

An item handle is a non-owning reference to a live QML object. Read and write its properties with
native syntax, and call its methods directly:

```roxal
var label  = engine.find("status")
var slider = engine.find("volume")

label.text = "Ready"          # write a property
var v = slider.value          # read a property
label.color = "#93c5fd"

slider.increase()             # call a Q_INVOKABLE / QML method
```

Values convert automatically across the boundary: numbers, bools, strings, **lists ↔ JS arrays**,
**dicts ↔ JS objects**, Roxal `vector`, and nested combinations. A QML method returning another item
gives you back an item handle.

For property/method names that aren't valid Roxal identifiers (e.g. `"z-order"`) or for fully dynamic
access, use the escape hatches:

```roxal
qt.set(item, "z-order", 3)
var x = qt.get(item, "some-prop")
var r = qt.call(item, "doThing", [arg1, arg2])   # args as a list
```

Accessing a missing property/method, or using a destroyed item, raises a **catchable Roxal
exception**.

---

## Reacting to Qt signals (events & callbacks)

A Qt/QML signal (a button's `clicked`, a slider's `valueChanged`, a custom QML `signal`) reaches
Roxal in **two interchangeable styles**.

**Callback style** — mirrors QML's `on…` handlers. Write `item.on<SignalName>(handler)`; the handler
runs synchronously with the signal's arguments:

```roxal
var btn = engine.find("okButton")
btn.onClicked(proc():
  print("clicked!")
)

var slider = engine.find("volume")
slider.onValueChanged(proc(v):
  print("volume = {v}")           # handler arity = signal arg count
)
```

**Event style** — Roxal-reactive. The bare signal name is an **event** you observe with
`when … occurs`; the signal's arguments arrive as the event payload (keyed by the Qt parameter names):

```roxal
when btn.clicked occurs:
  print("clicked (as an event)")

when slider.valueChanged occurs as e:
  print("volume -> {e.value}")
```

Both can coexist on the same signal. For dynamic / non-identifier signal names there are escape
hatches: `qt.connect(item, "clicked", handler)` (returns a connection id), `qt.on(item, "clicked")`
(returns the event), and `qt.disconnect(conn)`.

> Handlers run on the single UI thread. A handler that calls `engine.quit()` unblocks `run()`.

### A tiny end-to-end example

```qml
// counter.qml
import QtQuick
import QtQuick.Window
import QtQuick.Controls

Window {
    objectName: "win"; visible: true; width: 240; height: 140
    Column {
        anchors.centerIn: parent; spacing: 12
        Button   { objectName: "inc"; text: "Increment" }
        Text     { objectName: "out"; text: "count: 0" }
    }
}
```

```roxal
import qt

var engine = qt.Engine()
engine.load('counter.qml')

var out   = engine.find("out")
var count = 0

var inc = engine.find("inc")
inc.onClicked(proc():
  count = count + 1
  out.text = "count: {count}"
)

engine.run()
```

---

## List models

To render a collection in a `ListView`/`Repeater`, build a **`qt.ListModel`** whose rows are Roxal
**objects** — the row type's public properties become the model's **roles** (so a delegate binds them
by name). No `roleNames()` boilerplate, no `data(row, role)` switch.

```roxal
import qt

# A row type — its public properties are the columns/roles.
type Task object:
  var name :string
  var done :bool

var engine = qt.Engine()

var model = qt.ListModel(Task, [Task("Weld seam", false), Task("Inspect", true)])
engine.set_context_property("tasks", model)   # expose BEFORE load()
engine.load('tasks.qml')

engine.run()
```

```qml
// tasks.qml
import QtQuick
import QtQuick.Window
import QtQuick.Controls

Window {
    objectName: "win"; visible: true; width: 300; height: 320
    ListView {
        anchors.fill: parent
        model: tasks                       // the context property
        delegate: Row {
            spacing: 8
            CheckBox { checked: model.done; onToggled: model.done = checked }
            Text     { text: model.name }   // model.<property>
        }
    }
}
```

- The row type may be an **object type or an interface**; rows must be of that type (or a subclass /
  implementer).
- A **`const`** property is a **read-only** role; a writable property is editable from the UI (a
  delegate writing `model.done = x` flows back into the Roxal row object).

Drive the model from Roxal:

| Call | Effect |
|---|---|
| `model.append(row)` / `insert(i, row)` / `remove(i)` / `move(from, to)` / `clear()` | structural changes (the view updates) |
| `model.set_rows(list)` | replace all rows |
| `model.begin_reset()` … `model.end_reset()` | batch many changes into one reset |
| `model.row(i)` / `model.count()` | read access |
| `model.row_changed(i)` / `model.cell_changed(i, role)` | announce that you edited a row's properties |
| `model.set(i, role, value)` | set a cell value and notify in one call |

> List-model change notification is **explicit** — after editing a row object's properties, call
> `model.row_changed(i)` (or `cell_changed`/`set`). (A single bindable object, below, auto-notifies;
> a list of many rows is announced explicitly to avoid one subscription per cell.)

---

## Bindable objects (the Q_PROPERTY analogue)

To expose a single Roxal object as a **bindable backend** (app state, a controller, settings), pass it
to `set_context_property`. Its public properties become QML-bindable: QML reads them, binds to them,
and writes them back — and **Roxal-side edits auto-update the bindings**.

```roxal
import qt

type AppState object:
  var title   :string = "Ready"
  var volume  :int = 50
  const version :string = "1.0"      # const → read-only in QML

var engine = qt.Engine()
var state = AppState()
engine.set_context_property("app", state)   # auto-wraps a plain object
engine.load('app.qml')

# Edit the object anywhere; the bindings update automatically:
var btn = engine.find("resetBtn")
btn.onClicked(proc():
  state.title = "Reset"     # → the bound label updates
  state.volume = 0          # → the bound slider moves
)

engine.run()
```

```qml
// app.qml
import QtQuick
import QtQuick.Window
import QtQuick.Controls

Window {
    objectName: "win"; visible: true; width: 320; height: 200
    Column {
        anchors.centerIn: parent; spacing: 12
        Text   { text: app.title }                              // ← reads app.title
        Slider { from: 0; to: 100; stepSize: 1
                 value: app.volume                              // ← reads app.volume
                 onMoved: app.volume = value }                  // → writes app.volume (gated)
        Button { objectName: "resetBtn"; text: "Reset" }
    }
}
```

- **Reads/binds** come from the object's current property values; **QML writes** flow back through the
  property (a `const` property is read-only and the write is ignored).
- **Roxal-side assignments auto-push** (`state.title = "X"` updates the binding). If you mutate a
  *contained* collection **in place** (e.g. `state.items.append(x)` — no reassignment), call
  **`qt.notify(state, "items")`** (or `qt.notify(state)` for all) to force a push.

---

## Roxal signals vs Qt signals

This is the one piece of vocabulary that trips people up, because **both** Roxal and Qt use the word
"signal" for **different** concepts:

- **Roxal `signal`** is a **dataflow** primitive — a continuously-valued stream that other dataflow
  nodes consume. It has nothing to do with UI events.
- **A Qt signal** is an **event notification** emitted by a QObject (e.g. `Button.clicked`,
  `Slider.valueChanged`).

To avoid conflating them, the `qt` module **never** surfaces a Qt signal as a Roxal dataflow
`signal()`. Instead, **Qt signals are mapped to Roxal events (and callbacks)**:

| Qt concept | In Roxal (`qt` module) |
|---|---|
| Qt signal → slot (callback) | `item.onSignalName(handler)` — a synchronous callback |
| Qt signal observed reactively | `when item.signalName occurs [as e]` — a Roxal **event** (payload = the signal's args) |
| Roxal dataflow `signal()` | **unchanged** — still pure dataflow, unrelated to Qt |

So when you read "the button's clicked **signal**", that's a **Qt** signal, and in Roxal you handle it
as an event/callback — not with Roxal's `signal()` dataflow. (Mapping a changeable Qt property's
`NOTIFY` to a continuous Roxal dataflow `signal()` is a possible future direction, but is deliberately
*not* what the current bridge does.)

---

## Notes & gotchas

- **Single thread:** the VM and all Qt objects share the main thread. Don't touch Qt objects from
  Roxal **actors**.
- **`objectName`, not `id`:** QML `id`s aren't runtime-findable; set `objectName` on items you reach
  from Roxal.
- **Expose context properties *before* `load()`** so bindings see them when the QML is created.
- **Inline QML strings:** use single-quoted Roxal strings (double-quoted strings interpolate `{...}`).
- **Multi-line handlers:** write proc/closure bodies that contain statements (assignments) in the
  block form:

  ```roxal
  btn.onClicked(proc():
    state.count = state.count + 1
  )
  ```

See `examples/qt/` for runnable demos (`signals`, `listmodel`, `bindable`).
