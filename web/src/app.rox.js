// The Roxal side of the demo app, as a string so it ships with the bundle.
//
// Everything here is a SIGNAL NETWORK -- there is no update loop. Each line
// builds a node in a dataflow graph that the engine evaluates on 20 Hz
// boundaries, which is what Roxal is actually for. Nothing below knows React
// exists.
export default `import web
import math

// A heater and the thing it is heating, expressed as a SIGNAL NETWORK.
// There is no update loop anywhere below: each line builds a node in a dataflow
// graph, and the engine evaluates the whole graph on 20 Hz boundaries.

// An ordinary func. Called with a signal it is NOT invoked -- it becomes a node
// in the graph that re-evaluates whenever its input changes.
func duty_for(e :real) -> real:
  if e > 40.0:
    return 1.0
  return e / 40.0

var setpoint    = signal(20, 20.0, "setpoint")
var temperature = signal(20, 20.0, "temperature")

// The plant: a first-order lag toward the setpoint. temperature[-1] is the
// previous value, so this line IS the difference equation.
temperature <- temperature[-1] + (setpoint - temperature[-1]) * 0.08

// Derived signals. These are graph nodes, not values computed once -- calling a
// func with a signal builds a node too, it does not call the func.
var error    = setpoint - temperature
var drift    = math.abs(error)
var heating  = drift > 0.5
var duty     = duty_for(drift)

type Oven object:
  // Signal properties cannot be default-constructed, so they get a placeholder
  // and are connected to the network below.
  var setpoint    :signal = signal(20, 0.0)
  var temperature :signal = signal(20, 0.0)
  var error       :signal = signal(20, 0.0)
  var duty        :signal = signal(20, 0.0)
  var heating     :signal = signal(20, false)
  var phase :string = "idle"

  proc preheat():
    setpoint.set(150.0)
    phase = "preheat"

  proc reflow():
    setpoint.set(240.0)
    phase = "reflow"

  proc cool():
    setpoint.set(20.0)
    phase = "cool"

var oven = Oven()
oven.setpoint    = setpoint
oven.temperature = temperature
oven.error       = error
oven.duty        = duty
oven.heating     = heating

// Discrete transitions are events on a derived signal; the continuous behaviour
// stays in the network. This is the split the language is built around.
when heating becomes false:
  oven.phase = "settled at " + string(int(temperature.value))

web.expose("oven", oven)
print("network built; engine evaluating at 20 Hz")
web.serve()
`;
