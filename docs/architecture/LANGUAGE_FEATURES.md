# Omni — Language Feature Specification

**Status:** Stable
**Owner:** Omni Systems Dev Team
**Last Updated:** 2026-09-19

This document defines the user-facing language feature set for Omni. It is the
contract that the compiler and runtime (specified in `DESIGN.md` and `LAWS.md`)
must preserve.

Omni is designed to balance Pythonic simplicity with extreme runtime flexibility
and high-performance concurrency.

---

## 1. Radical Dynamism ("Anything Becomes Anything")

- **Trait Injection:** Dynamically add, remove, or override methods and properties on *any* object or class at runtime without affecting existing instances unless intended.
- **Universal Callability:** Any object can become a function. Assign a `call` trait to a list, map, or custom object to make it executable via `obj()`.
- **Universal Iterability:** Any object can become a loopable sequence. Assign a `next` trait to make it compatible with `for...in` loops and spread operators.
- **Implicit Morphing:** Objects define how they cast to other types (e.g., `to_int`, `to_str`). The runtime seamlessly morphs types based on context without requiring explicit casting syntax.
- **Runtime Shape-Shifting:** Objects can dynamically change their underlying memory layout (hidden classes/shapes) at runtime, with the JIT automatically adapting and re-optimizing.
- **Deep Reflection:** First-class APIs to inspect, traverse, and mutate the prototype/trait chain, object shapes, and method resolutions at runtime.

---

## 2. Core Syntax & Ergonomics (Pythonic Ease)

- **Indentation-Based:** Clean, brace-free, semicolon-free syntax.
- **Zero Boilerplate:** No explicit imports for standard operations; standard library is globally available or auto-imported.
- **Deep Pattern Matching:** First-class `match` statements that destructure complex, nested objects, lists, and dictionaries dynamically.
- **Advanced Comprehensions:** List, dictionary, set, and generator comprehensions with multiple `if` filters and nested loops.
- **First-Class Functions & Closures:** Functions are objects. Full support for closures, higher-order functions, and lambda expressions.
- **Operator Overloading:** Seamlessly overload any operator (`+`, `==`, `[]`, `()`) for custom shapes and types.
- **Named & Default Arguments:** Flexible function signatures with keyword arguments, default values, and variadic parameters (`*args`, `**kwargs`).

---

## 3. Concurrency & Parallelism (No GIL)

- **True Free-Threading:** No Global Interpreter Lock. Multiple threads can execute Omni code simultaneously on multiple CPU cores.
- **M:N Threading (Lightweight Tasks):** Spawn millions of concurrent tasks (`spawn compute()`) mapped efficiently to a smaller pool of OS threads.
- **Native Async/Await:** First-class `async` and `await` syntax for non-blocking I/O, seamlessly integrating with the M:N task scheduler.
- **Fine-Grained `sync` Blocks:** Instead of manual mutexes, use `sync { ... }` blocks. The runtime automatically handles fine-grained, scoped locking for shared mutable state, preventing deadlocks.
- **Message-Passing Channels:** First-class, typed channels (`chan`) for safe, lock-free communication between tasks (inspired by Go/Clojure).
- **Isolated Regions:** Optional memory regions that guarantee single-thread access, allowing the JIT to completely eliminate synchronization overhead for isolated data.

---

## 4. Memory & Resource Management

- **Concurrent Generational GC:** An invisible, highly optimized garbage collector with sub-millisecond "Stop-The-World" pauses. Handles circular references automatically.
- **Deterministic Resource Scoping:** `using` blocks or `defer` statements ensure immediate, deterministic cleanup of external resources (files, network sockets, locks) without waiting for the GC.
- **Weak References & Finalizers:** Built-in `WeakRef` and `FinalizationRegistry` for building memory-efficient caches and managing external C/C++ resources.
- **Zero-Copy Slicing:** Slicing strings, lists, and buffers creates views rather than copying memory, unless explicitly mutated (Copy-on-Write).

---

## 5. Diagnostics & Developer Experience

- **Explainable Error Engine:** Errors are not just stack traces. They include context-aware diagnostics, exact variable states at the time of failure, and semantic auto-fix suggestions.
- **Interactive REPL Recovery:** In dev mode, a crash drops you into an interactive shell at the exact line of failure. You can inspect/mutate variables and type `resume` to continue execution.
- **State Snapshots:** Exception tracebacks automatically capture and display the value of all local variables at every frame in the stack.
- **Built-in Profiler & Tracer:** Zero-config performance profiling and execution tracing accessible via standard library calls or REPL commands.

---

## 6. Standard Library & Interop

- **Batteries Included:** Comprehensive standard library covering HTTP/HTTPS, JSON, XML, File I/O, Cryptography, Regex, and Async networking.
- **Seamless FFI (Foreign Function Interface):** Directly call C, C++, and Rust functions without writing boilerplate wrapper code. The runtime handles ABI translation, memory ownership, and exception bridging.
- **Native Extension API:** A clean, versioned C-API for writing high-performance Omni extensions in C/C++/Rust.
- **Polymorphic Core Functions:** Standard functions adapt to the shape of the input. `len()`, `map()`, and `filter()` work natively on strings, lists, maps, and any custom object with the corresponding traits.
