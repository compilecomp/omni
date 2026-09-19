// core/bytecode/bytecode_module.hpp
//
// A compiled bytecode module — the unit of compilation and loading.
//
// Purpose:
//   Implements the runtime representation of a loaded Omni module. A
//   BytecodeModule holds the instruction stream, constant pool, symbol
//   resolution table, function descriptors, and metadata required by
//   the interpreter and the JIT tiers.
//
// Invariants:
//   - A module is immutable once published. New versions are new modules.
//   - The instruction stream is a flat array of Instructions.
//   - The constant pool is a flat array of TaggedValues.
//   - All symbols referenced by instructions are interned; the module
//     stores SymbolId arrays for property names, method names, etc.
//   - Loading verifies integrity (Rule 105): instruction stream length
//     fits in MAX_BYTECODE_LENGTH, constant pool indices are in range,
//     branch targets are within the module.
//
// Edge cases:
//   - A function within a module may be entered at any pc that is a
//     valid opcode boundary (verified at load time).
//   - The constant pool can hold any TaggedValue; some entries are
//     pre-resolved (e.g., interned method names) and others are
//     resolved at runtime (e.g., global references).
//
// Cross-references:
//   - LAWS.md Rule 105 (Profiles, Bytecode, and Caches Are Untrusted)
//   - LAWS.md Rule 108 (Serialized Artifacts Must Be Verified)
//   - LAWS.md Rule 107 (Compatibility Manifest)
//   - DESIGN.md §4.1 (semantic bytecode)

#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "core/bytecode/instruction.hpp"
#include "core/common/result.hpp"
#include "core/common/types.hpp"
#include "core/object_model/tagged_value.hpp"

namespace omni::bytecode {

/// Function descriptor within a module.
struct FunctionDesc {
    common::SymbolId name;          // interned name
    uint32_t entry_pc;              // pc of the first instruction
    uint16_t param_count;
    uint16_t local_count;           // including params
    uint16_t default_count;         // number of default-arg expressions
    uint16_t max_registers;         // peak register usage (for Tier 0)
    bool is_async;                  // async function (has await points)
    bool is_generator;              // generator function (has yield)
};

/// Exception handler table entry.
struct HandlerEntry {
    uint32_t try_start_pc;
    uint32_t try_end_pc;            // exclusive
    uint32_t handler_pc;
    common::SymbolId exception_type;  // 0 = catch all
};

/// Constant pool entry. May be a primitive value, an interned symbol,
/// or a forward reference resolved at load time.
struct ConstEntry {
    object_model::TaggedValue value;
};

class BytecodeModule {
public:
    BytecodeModule(uint32_t module_id, std::vector<Instruction>&& code)
        : module_id_(module_id), code_(std::move(code)) {}

    [[nodiscard]] uint32_t module_id() const noexcept { return module_id_; }
    /// Read-only access to the instruction stream. The returned span is
    /// const; quickening writes go through code_atomic_mut() (B9 fix).
    [[nodiscard]] std::span<const Instruction> code() const noexcept {
        return {code_.data(), code_.size()};
    }
    /// Atomic-write access for the quickening/fusion engine (B9 fix).
    /// Each Instruction is read/written atomically. Readers (the
    /// interpreter dispatch loop) must use code() and accept that they
    /// may see either the old or new instruction at any given site;
    /// they will never see a torn read because each Instruction is a
    /// single atomic 32-bit load.
    /// DESIGN.md §5.8: "quickening overlay atomically updated".
    [[nodiscard]] std::span<std::atomic<uint32_t>> code_atomic_mut() noexcept {
        // Reinterpret the Instruction storage as atomic uint32 words.
        // Each Instruction is 3 bytes padded to 4; the 4th byte is
        // unused padding. Atomic 32-bit writes are atomic on all
        // supported platforms.
        return {reinterpret_cast<std::atomic<uint32_t>*>(code_.data()),
                code_.size()};
    }
    /// Used by the constructor only — before publication. After the
    /// module is published, all writes go through code_atomic_mut().
    [[nodiscard]] std::span<Instruction> code_init() noexcept {
        return {code_.data(), code_.size()};
    }
    [[nodiscard]] uint32_t length() const noexcept {
        return static_cast<uint32_t>(code_.size());
    }

    /// Constant pool access.
    [[nodiscard]] const std::vector<ConstEntry>& constants() const noexcept { return constants_; }
    void add_constant(ConstEntry c) { constants_.push_back(c); }

    /// Function table access.
    [[nodiscard]] const std::vector<FunctionDesc>& functions() const noexcept { return functions_; }
    void add_function(FunctionDesc f) { functions_.push_back(f); }

    /// Exception handler table access.
    [[nodiscard]] const std::vector<HandlerEntry>& handlers() const noexcept { return handlers_; }
    void add_handler(HandlerEntry h) { handlers_.push_back(h); }

    /// Look up the handler covering a given pc, or nullptr.
    [[nodiscard]] const HandlerEntry* find_handler(uint32_t pc, common::SymbolId exc_type) const noexcept {
        for (const auto& h : handlers_) {
            if (pc >= h.try_start_pc && pc < h.try_end_pc) {
                if (h.exception_type == common::NULL_SYMBOL || h.exception_type == exc_type) {
                    return &h;
                }
            }
        }
        return nullptr;
    }

    // --- Per-pc side tables for instructions that need more than two
    // operands. The 24-bit instruction format gives us only (opcode, a, b);
    // for GET_PROP/SET_PROP we need a property name, and for GET_FIELD/
    // SET_FIELD we need a slot index. These are stored in per-pc maps
    // rather than expanding the instruction encoding (B2-4 fix
    // acknowledged this need).
    //
    // These maps are populated at module-construction time (by the
    // frontend or by hand-built test modules) and are read-only after
    // the module is loaded. Lookups are O(1).

    void set_prop_name(uint32_t pc, common::SymbolId name) {
        prop_name_at_pc_[pc] = name;
    }
    [[nodiscard]] common::SymbolId prop_name_at(uint32_t pc) const noexcept {
        auto it = prop_name_at_pc_.find(pc);
        return it != prop_name_at_pc_.end() ? it->second : common::NULL_SYMBOL;
    }

    void set_field_slot(uint32_t pc, uint32_t slot) {
        field_slot_at_pc_[pc] = slot;
    }
    [[nodiscard]] uint32_t field_slot_at(uint32_t pc) const noexcept {
        auto it = field_slot_at_pc_.find(pc);
        return it != field_slot_at_pc_.end() ? it->second : 0;
    }

private:
    uint32_t module_id_;
    std::vector<Instruction> code_;
    std::vector<ConstEntry> constants_;
    std::vector<FunctionDesc> functions_;
    std::vector<HandlerEntry> handlers_;
    std::unordered_map<uint32_t, common::SymbolId> prop_name_at_pc_;
    std::unordered_map<uint32_t, uint32_t> field_slot_at_pc_;
};

}  // namespace omni::bytecode
