
# Roxal - A Programming Language for AI & Robotics

(Compiler, Bytecode Virtual Machine & Runtime)

Features:
  - Easy to learn syntax (similarities with Python)
  - Static & Dynamic typing
  - Builtin types aimed at AI and Robotics - vector/matrix/tensor
  - Physical unit literals (eg. `5kg * 9.81m/s²` = `49.05N`)
  - Signals & Dataflow engine (signal & event types)
  - Object-Oriented (object types, inheritance, interfaces)
  - Safety - transitive const (via MVCC) & GC-based memory management
  - First class functions (closures)
  - Builtin library support for DDS (ROS interop) & gRPC (no codegen required)
  - Builtin library support for AI model inference (via ONNX Runtime)
  - Distribuited Compute (instantiate actors remotely)
  - VM integratable into Real-time control loops via runFor(deadline)

See [Roxal-for-devs](roxal-for-devs.md) for an overview.

Note: This is the core language and libraries without any actual proprietary interface for robot control.

__IN DEVELOPMENT - BETA__
