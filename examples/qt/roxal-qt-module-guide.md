# Roxal + Qt/QML — Building UIs from Roxal

> ⚠️ **Experimental.** The `qt` module is new and not yet exercised by any large application, so
> expect missing features, rough edges, and latent bugs. The API may change.

The optional **`qt` module** lets you build **QML / QtQuick** user interfaces and drive them from
Roxal. You author the UI in QML as usual; from Roxal you load it, find items, read and write their
properties, call their methods, react to their signals, fill list models, and bind plain Roxal objects
to QML.

This guide assumes you know a little QML and just want to drive it from Roxal. Two things up front:

- **You block explicitly.** A window being open doesn't keep the program running — you call
  `engine.run()`, which returns when the last window closes (or the UI requests a quit).

> **Heads-up on the word "signal":** Roxal has a `signal` type (dataflow), and Qt also has "signals" —
> they're unrelated. The `qt` module surfaces **Qt signals as Roxal events and callbacks**, never as
> Roxal dataflow `signal()`s. See [Roxal signals vs Qt signals](#roxal-signals-vs-qt-signals).

## Building with Qt enabled

The `qt` module is **off by default**. Configure with `ROXAL_ENABLE_QT=ON` and point CMake at a
desktop Qt 6:

```sh
cmake -B build/ -DROXAL_ENABLE_QT=ON -DCMAKE_PREFIX_PATH=/opt/Qt/6.8.3/gcc_64
cmake --build build/ -j4
```

Run a script the usual way: `./build/roxal myapp.rox`. For headless/CI runs, set
`QT_QPA_PLATFORM=offscreen`.

### Running and shipping qt scripts

The `qt` module is optional and loaded **at runtime**, so a Qt-enabled build produces a
`libroxalqt.so` next to the `roxal` binary. To run a script that does `import qt` you need
**Qt 6 installed** and that `libroxalqt.so` present (it's found automatically when kept beside
the binary). Scripts that don't `import qt` run fine without Qt — the base `roxal` doesn't
depend on it. If you `import qt` while Qt or the plugin is missing, you get a clear error at
that import (not a crash): `import 'qt' failed: … libroxalqt.so … cannot open shared object
file`.

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
engine.run()               # blocks until the window closes / Qt.quit()
print('bye')
```

Your script blocks at `engine.run()` while the UI is up; it returns when the last window closes (or
QML calls `Qt.quit()` / Roxal calls `engine.quit()`), after which the script finishes.

You can also load QML from a string instead of a file:

```roxal
engine.load_string("import QtQuick\nimport QtQuick.Window\nWindow { visible: true; Text { text: 'hi' } }\n")
```

(Use **single-quoted** Roxal strings for inline QML — double-quoted strings interpolate `{...}`, which
clashes with QML's braces.)

---

## The Engine

`qt.Engine` loads QML and runs the UI. Its methods:

| Method | What it does |
|---|---|
| `qt.Engine()` | Create a UI engine. |
| `engine.load(path)` | Load QML from a file. The root object must be a `Window`. |
| `engine.load_string(qml)` | Load QML from an inline string. |
| `engine.run()` | Block until the last window closes / `Qt.quit()`. |
| `engine.run_for(ms)` | Pump the UI for ~`ms` and return (non-blocking; for interactive/REPL use). |
| `engine.quit()` | Ask `run()` to unblock (e.g. from a handler). |
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

### Where QML & assets are looked up

`engine.load("ui.qml")` resolves the path **relative to the `.rox` script that calls it** (not the
current working directory), so your app runs the same from any directory. If the file isn't found next
to the script, the **Roxal module search paths** are tried next (handy for a shared assets directory),
then the working directory. **Absolute paths** and **URLs** (`file:`, `qrc:`, `image:`, …) are used as
given. `engine.load_string(qml)` treats the inline QML as if it lived next to the script, so its
relative asset URLs resolve there too.

So: keep QML and assets next to your `.rox` and reference them by bare relative name.

### Driving a UI interactively (without `run()`)

`run()` hands the thread to the UI until the window closes — which is what an app wants, but not what
you want at a REPL, where you'd like to load a UI and then poke at it live. The catch: until the Qt
loop is pumped, a loaded window doesn't paint and queued handlers don't fire. Three calls let you
pump it **on demand** instead, so the script (or REPL) keeps control:

| Call | What it does |
|---|---|
| `qt.process_events(max_ms=nil)` | Service all pending Qt events once (repaint, queued handlers) and return. |
| `engine.run_for(ms)` | Pump for ~`ms` then return (e.g. to let an animation play a beat). |
| `qt.every(ms, fn)` | Call `fn()` ~every `ms` while the loop is pumped; returns a timer handle to `stop()`. |

```roxal
import qt
var e = qt.Engine()
e.load("ui.qml")
qt.process_events()            # the window maps and paints

var slider = e.find("volume")
slider.value = 80              # the write lands immediately…
qt.process_events()            # …and now you see it repaint

print(slider.value)           # reads are always live — no pump needed

# A periodic Roxal callback on the UI thread (fires while the loop is pumped):
var t = qt.every(500, proc():
  print("tick")
)
e.run_for(2000)                # ~4 ticks, then return
t.stop()
```

Property **reads** are live regardless; **writes** take effect in the item immediately — pumping is
what makes the repaint and any queued signal handlers actually happen. `qt.every`'s callback runs on
the main UI thread, so it may touch Qt items freely. (For a *continuously* live prompt — the window
responsive while you think — you'd integrate stdin into the event loop; these one-shots are the
simpler building block.)

### Choosing a Controls style

Qt Quick Controls ships several **styles** — `Basic` (the default), `Fusion`, `Material`,
`Universal`, `Imagine`. Pick one with `qt.set_style(name)` **before** loading any QML (it has no
effect once a control exists):

```roxal
import qt
qt.set_style("Fusion")          # or "Material", "Universal", …
var e = qt.Engine()
e.load("app.qml")
e.run()
```

It's a process-wide, app-level choice (raises on an unknown style name). For per-control theming on
top of a style (e.g. Material accent/colours), set the style's attached properties in QML.

---

## Items & properties

An item handle refers to a live QML object. Read and write its properties with native syntax, and call
its methods directly:

```roxal
var label  = engine.find("status")
var slider = engine.find("volume")

label.text = "Ready"          # write a property
var v = slider.value          # read a property
label.color = "#93c5fd"

slider.increase()             # call a method the object exposes
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

## Creating items dynamically

Sometimes the items you need aren't in the loaded QML — you want to spawn them at runtime (a marker
per detected object, a row of buttons, a node per robot joint). Compile a QML snippet once into a
**`qt.Component`**, then `create()` items from it under a chosen parent — Roxal's version of QML's
`Qt.createComponent` + `createObject`.

```roxal
# Compile once — from a .qml file…
var marker = engine.create_component("Marker.qml")
# …or from inline QML:
var marker = engine.create_component_string("""
    import QtQuick
    Rectangle { property string label: ""; width: 20; height: 20; radius: 10; color: "tomato" }
""")

var box = engine.find("box")                 # a container Item in the loaded QML

# Spawn an instance under the box, with initial property values:
var m = marker.create(box, {"label": "joint-1", "x": 10, "y": 40})
m.color = "steelblue"                        # it's an ordinary item handle afterward
```

- `create(parent, props)` returns an item handle just like `find()`. `props` (optional) is a dict of
  initial property values, applied **before** the item is completed so its bindings see them.
- With a **`parent`** item, the new item is parented to it — for ownership (it's destroyed with the
  parent) and, for visual items, for layout (it appears inside the parent). Pass **`nil`** to create a
  detached item (owned by the engine); parent it yourself later.
- A compile error in the QML (in `create_component*`) or a failed instantiation (in `create`) raises a
  **catchable Roxal exception**.

One `qt.Component` makes as many items as you like — compile in setup, `create()` in a loop. See
[examples/qt/dynamic.rox](dynamic.rox).

---

## Reacting to Qt signals (events & callbacks)

A Qt/QML signal (a button's `clicked`, a slider's `valueChanged`, a custom QML `signal`) reaches Roxal
in **two interchangeable styles**.

**Callback style** — mirrors QML's `on…` handlers. Write `item.on<SignalName>(handler)`; the handler
runs with the signal's arguments:

```roxal
var btn = engine.find("okButton")
btn.onClicked(proc():
  print("clicked!")
)

var slider = engine.find("volume")
slider.onValueChanged(proc(v):
  print("volume = {v}")           # the handler's params are the signal's args
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

> A handler that calls `engine.quit()` unblocks `run()`.

### When a handler raises

If a handler lets an exception escape, it's **not** swallowed: it aborts `engine.run()` and
surfaces as an uncaught exception (fail-loud), and any remaining handlers for that signal are
skipped. To keep the UI running instead, **catch inside the handler**:

```roxal
btn.onClicked(proc():
  try:
    doRiskyThing()
  except e:
    status.text = "Error: " + string(e)   # handled — run() keeps going
)
```

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
**objects** — the row type's public properties become the model's **roles**, so a delegate binds them
by name (no `roleNames()`/`data()` to write yourself).

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

> After editing a row object's properties directly, tell the view with `model.row_changed(i)` (or
> `cell_changed`/`set`). The structural calls above notify on their own.

### Sorting & filtering

To show the same rows **sorted and/or filtered** without changing the underlying model, wrap a
`qt.ListModel` in a **`qt.SortFilterModel`** and expose *that* to the view:

```roxal
var model = qt.ListModel(Task, [...])
var view  = qt.SortFilterModel(model)
view.sort_by("name")              # by a role, ascending
view.sort_by("priority", descending=true)
view.filter("name", "weld")       # rows whose `name` contains "weld" (case-insensitive)
view.clear_filter()
engine.set_context_property("tasks", view)   # the ListView binds the view; delegate still uses model.<role>
```

The view tracks the source live — appends/edits to `model` flow through. `view.count()` is the
visible row count and `view.row(i)` is the source object at visible index `i`. (v1 supports
role-based sort + substring filter; arbitrary Roxal predicates are a future addition.)

### Tree models

For hierarchical data in a QML `TreeView`, use **`qt.TreeModel`** — the same idea, but each node's
child nodes live in a **`children` list property** (which is structural, so it is *not* a role; the
node's *other* public properties are the roles). All operations take a **parent node** (or `nil` for
the root level):

```roxal
type Node object:
  var name :string
  var children :list = []        # subtree — excluded from the roles

var root = Node("base")
var tree = qt.TreeModel(Node, [root])   # roots: optional list of top-level nodes
tree.append(root, Node("link1"))        # add a child under `root`
tree.append(root, Node("link2"))
engine.set_context_property("scene", tree)
```

```qml
TreeView {
    model: scene
    delegate: TreeViewDelegate { text: name }    // `name` is the role
}
```

The API mirrors the list model but is node-addressed: `count(parent)`, `child(parent, i)`,
`parent_of(node)`, `append(parent, node)` / `insert(parent, i, node)` / `remove(parent, i)` /
`move(parent, from, to)` / `clear(parent)`, the `begin_reset()`/`end_reset()` batch, and
`node_changed(node)` / `cell_changed(node, role)` / `set(node, role, value)`. Mutate the tree
**through these calls** so the view is notified (v1 doesn't observe direct edits to a node's
`children` list).

### Table models

For a QML `TableView` with real columns, use **`qt.TableModel`**. Like the list model the rows are
Roxal objects, but you pick which properties are **columns** (and in what order), each with a
header — so a row type can carry more than the table shows:

```roxal
type Part object:
  var name   :string
  var qty    :int
  var status :string

# Columns: a [property, header] pair or a bare property name (header = the name).
var inv = qt.TableModel(Part, [["name", "Part"], "qty", ["status", "Status"]], [
  Part("Gripper", 3, "OK"),
  Part("Wrist sensor", 1, "Low"),
])
engine.set_context_property("inv", inv)
```

```qml
HorizontalHeaderView { id: header; syncView: table }   // titles come from the model's headers
TableView {
    id: table
    model: inv
    delegate: Rectangle {
        implicitWidth: 140; implicitHeight: 32
        Text { anchors.centerIn: parent; text: model.display }   // the cell value; column = the delegate's `column`
    }
}
```

A delegate binds the cell value as **`model.display`** (the column is the delegate's `column`); a
`HorizontalHeaderView` reads the column **headers** from the model. `nil` columns means "every
public property, in order". The rest of the API mirrors the list model — `count()`,
`column_count()`, `row(i)`, `append`/`insert`/`remove`/`move`/`clear`, `set_rows`, the
`begin_reset()`/`end_reset()` batch, and `row_changed(i)` / `cell_changed(i, column)` /
`set(i, column, value)` (where `column` is a column's property name). A writable column is
editable from the UI; a `const` one is read-only.

---

## Bindable objects

To expose a single Roxal object as a **bindable backend** (app state, a controller, settings), pass it
to `set_context_property`. Its public properties become QML-bindable: QML reads them, binds to them,
and writes them back — and **Roxal-side edits update the bindings automatically**.

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
                 onMoved: app.volume = value }                  // → writes app.volume
        Button { objectName: "resetBtn"; text: "Reset" }
    }
}
```

- **Reads/binds** come from the object's current property values; **QML writes** flow back into the
  object (a `const` property is read-only and the write is ignored).
- **Roxal-side assignments auto-push** (`state.title = "X"` updates the binding). If you mutate a
  *contained* collection **in place** (e.g. `state.items.append(x)` — no reassignment), call
  **`qt.notify(state, "items")`** (or `qt.notify(state)` for all) to force a push.
- **Computed properties bind too.** A property with `get:`/`set:` accessors is a role: QML reads the
  getter's value and writes flow through the setter (a get-only property is read-only). Its
  auto-push follows its own `_<name>` backing field, so a getter that derives from *other* fields
  won't refresh on its own — call `qt.notify(state, "name")` after changing those.
- **Methods are callable from QML.** The object's public methods are exposed as callable values, so
  QML can invoke them directly — `onClicked: app.reset()`, `onAccepted: app.submit(text)` — and a
  `func`'s return value comes back to QML. Args and the return convert like everything else.

```roxal
type AppState object:
  var title :string = "Ready"
  proc reset(): title = "Ready"
  proc submit(s :string): title = "Sent: " + s
  func greeting() -> string: return "Hi, " + title
```

```qml
Button   { text: "Reset";  onClicked: app.reset() }
TextField { onAccepted: app.submit(text) }
Text     { text: app.greeting() }          // a func's result reaches QML
```

> Only public, non-overloaded methods are exposed (an overloaded name is skipped, since the call is
> resolved by name). Method bodies run on the main thread, same as everything else here.

---

## Pixel frames: FrameView (live images, framebuffers)

For showing **pixels you compute or receive** — camera frames off DDS, depth maps, NN masks,
or a software-rendered framebuffer — the module registers a QML item type, `FrameView`
(`import Roxal`), that displays a Roxal **image tensor**: `uint8`, shape `[H, W, C]` with 1
(grayscale), 3 (RGB) or 4 (RGBA) channels. `media.Image` pixels are already exactly that
(`img.to_tensor()`); float tensors convert with `astype`.

```qml
import Roxal
FrameView { objectName: "fb"; smooth: false; anchors.fill: parent }
```

```roxal
var fb = engine.find("fb")
fb.present(frame)        # frame: uint8 [H, W, C] tensor — repaints the item
fb.frame = frame         # same thing as a property; read it back → tensor
fb.framesPresented       # frames shown so far (diagnostics/tests)
fb.frame = nil           # clear
```

The frame is uploaded as a **scene-graph texture at its own size** and scaled to the item by
the renderer, so presenting is cheap even when the window is large. `smooth: false` gives
crisp nearest-neighbor pixels (low-res/retro sources); leave it on (the default) for camera
feeds. Presenting ~35 frames/sec is no problem — the cost that matters is producing the
pixels, not showing them.

Two companions round this out:

```roxal
qt.set_render_backend("software")   # render the scene graph on the CPU — no GPU/driver
                                    # needed (robots, containers, CI). Call before load().
var shot = engine.grab_window()     # window → uint8 [H, W, C] tensor (screenshot/tests)
```

Everything — FrameView included — renders identically on the software backend; the
`qt_frameview` test proves it pixel-for-pixel headlessly (`QT_QPA_PLATFORM=offscreen`).
And for **key-driven** pixel apps (games!), handle `Keys` in QML and forward press/release
to Roxal through a QML signal; `qt.post_key(win, code, pressed)` synthesizes keys for
headless tests (see `tests/qt_keys.rox`). The runnable demo is
[framebuffer.rox](framebuffer.rox) — a palette-cycling plasma presented at ~12 fps.

## Roxal signals vs Qt signals

This is the one piece of vocabulary that trips people up, because **both** Roxal and Qt use the word
"signal" for **different** things:

- **Roxal `signal`** is a **dataflow** primitive — a continuously-valued stream that other dataflow
  nodes consume. It has nothing to do with UI events.
- **A Qt signal** is an **event notification** emitted by a UI object (e.g. `Button.clicked`,
  `Slider.valueChanged`).

The `qt` module **never** surfaces a Qt signal as a Roxal dataflow `signal()`. Instead, **Qt signals
become Roxal events and callbacks**:

| Qt concept | In Roxal (`qt` module) |
|---|---|
| Qt signal → slot (callback) | `item.onSignalName(handler)` — a callback |
| Qt signal observed reactively | `when item.signalName occurs [as e]` — a Roxal **event** (payload = the signal's args) |
| Roxal dataflow `signal()` | **unchanged** — still pure dataflow, unrelated to Qt |

So when you read "the button's clicked **signal**", that's a **Qt** signal, and in Roxal you handle it
as an event/callback — not with Roxal's `signal()` dataflow.

---

## Quieting Qt messages

Qt and QML print diagnostics (warnings, QML `TypeError`s, etc.) to **stderr** by default, which can
clutter a console you're also using for `print()`. Silence them or send them to a file:

```roxal
qt.log_to_file('app.log')   # Qt/QML messages → append to app.log (out of the console)
qt.log_silence()            # discard all Qt/QML messages (fatal errors still abort)
qt.log_to_stderr()          # restore the default (messages → stderr)
```

Call one early (e.g. right after `import qt`).

## Notes & gotchas

- **Don't touch Qt from actors:** the UI runs on one thread; only use Qt items from your main flow,
  not from Roxal **actors** (or it will raise an exception).
- **`objectName`, not `id`:** QML `id`s aren't runtime-findable; set `objectName` on items you reach
  from Roxal.
- **Expose context properties *before* `load()`** so bindings see them when the QML is created.
- **Inline QML strings:** use single-quoted Roxal strings (double-quoted strings interpolate `{...}`).
- **Multi-line handlers:** write a handler whose body is a statement (like an assignment) in block
  form:

  ```roxal
  btn.onClicked(proc():
    state.count = state.count + 1
  )
  ```

See `examples/qt/` for runnable demos (`hello`, `signals`, `listmodel`, `bindable`).
