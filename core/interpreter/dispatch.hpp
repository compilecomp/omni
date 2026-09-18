// core/interpreter/dispatch.hpp
//
// Dispatch table and dispatch strategy for the Tier 0 interpreter.
//
// Purpose:
//   Implements DESIGN.md §5.2: there are three dispatch tables
//   (generic/semantic, quickened, fused), plus IC miss stubs and guard
//   failure stubs. The interpreter uses computed-goto on platforms that
//   support it (GCC labels-as-values), falling back to a switch on others.
//
// Invariants:
//   - Each Opcode has exactly one handler in each table where applicable.
//   - The dispatch table is read-only after construction (Rule 61: no
//     hot-path mutations).
//   - The handler signature is uniform: void(InterpFrame&, Interpreter&)
//     — handlers read operands from the current instruction and may
//     advance the pc.
//
// Cross-references:
//   - DESIGN.md §5.2 (dispatch tables)
//   - LAWS.md Rule 21 ([[likely]]/[[unlikely]] on PGO-driven branches)
//   - LAWS.md Rule 61 (no allocations, no virtual dispatch in hot path)
//   - LAWS.md Rule 96 (Tier 0 universal fallback)

#pragma once

#include <array>
#include <cstdint>

#include "core/bytecode/opcode.hpp"
#include "core/common/types.hpp"

namespace omni::interpreter {

class Interpreter;
class InterpFrame;

/// Handler function type. Takes a reference to the current frame and
/// the interpreter; reads operands from the frame's current pc.
using Handler = void (*)(InterpFrame& frame, Interpreter& interp);

class DispatchTable {
public:
    constexpr DispatchTable() = default;

    /// Register a handler for an opcode. The same handler may serve
    /// multiple opcodes (e.g., a no-op fallback for fused opcodes that
    /// are not yet implemented).
    constexpr void set(bytecode::Opcode op, Handler h) noexcept {
        handlers_[static_cast<uint8_t>(op)] = h;
    }
    [[nodiscard]] constexpr Handler get(bytecode::Opcode op) const noexcept {
        return handlers_[static_cast<uint8_t>(op)];
    }

private:
    /// 256 handlers, one per possible 8-bit opcode value. Uninitialized
    /// handlers are null; dispatch checks for null and falls back.
    std::array<Handler, 256> handlers_{};
};

}  // namespace omni::interpreter
