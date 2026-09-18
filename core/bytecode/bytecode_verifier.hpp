// core/bytecode/bytecode_verifier.hpp
//
// Bytecode verifier.
//
// Purpose:
//   Implements Laws Rule 105: "Profile data, serialized IR, caches, and
//   bytecode inputs must be validated before use. Malformed inputs must
//   not cause: undefined behavior, memory corruption, arbitrary code
//   execution, VM crashes, silent miscompilation."
//
//   The verifier runs at module load time. It checks:
//     - Instruction stream length fits MAX_BYTECODE_LENGTH.
//     - Every pc is a valid opcode boundary.
//     - Register operands are within FRAME_REGISTER_COUNT.
//     - Branch targets are within the module's instruction stream.
//     - Constant pool indices are in range.
//     - Function entry PCs are at valid opcode boundaries.
//     - Exception handler ranges are non-overlapping (or, if overlapping,
//       are properly nested) and within the code stream.
//     - Try-end PC is exclusive and > try-start PC.
//     - Opcodes are valid (in the Opcode enum range).
//
// Invariants:
//   - Verification is read-only: it does not mutate the module.
//   - Verification is total: every reachable code path returns either
//     success or a specific error code.
//
// Cross-references:
//   - LAWS.md Rule 105 (untrusted bytecode)
//   - LAWS.md Rule 63 (resilient to malformed input)
//   - LAWS.md Rule 58 (no silent fallbacks)

#pragma once

#include "core/bytecode/bytecode_module.hpp"
#include "core/common/result.hpp"

namespace omni::bytecode {

class BytecodeVerifier {
public:
    /// Verify a module. Returns success or an error.
    [[nodiscard]] static common::Result<void> verify(const BytecodeModule& module) noexcept;

private:
    // Static-only utility class.
    BytecodeVerifier() = delete;
};

}  // namespace omni::bytecode
