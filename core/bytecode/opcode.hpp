// core/bytecode/opcode.hpp
//
// Opcode enumeration for the three bytecode layers.
//
// Purpose:
//   Implements DESIGN.md §4: semantic, quickened, and fused bytecode.
//   Each layer is a separate enumeration namespace but shares the same
//   8-bit opcode space (semantic in [0, 127], quickened in [128, 223],
//   fused in [224, 255]).
//
// Invariants:
//   - Opcode is exactly 8 bits (the 24-bit instruction packs opcode in
//   - the high byte).
//   - Each opcode has a stable numeric value. New opcodes are appended;
//     none are reused across Omni versions (Laws Rule 31: cache versioning).
//   - Every opcode has a corresponding handler in the Tier 0 interpreter
//     (Laws Rule 96: Tier 0 is the universal correctness fallback).
//
// Cross-references:
//   - DESIGN.md §4.1 (semantic bytecode list)
//   - DESIGN.md §4.2 (quickened bytecode list)
//   - DESIGN.md §4.3 (fused bytecode list)
//   - LAWS.md Rule 31 (no persistent state without versioning)
//   - LAWS.md Rule 96 (Tier 0 universal fallback)

#pragma once

#include <cstdint>

namespace omni::bytecode {

enum class Opcode : uint8_t {
    // === Semantic opcodes (DESIGN.md §4.1) — range [0, 127] ===
    // Values are explicitly assigned; never renumber.
    Invalid            = 0,
    /// No-op. Used by the fusion engine to mark slots that have been
    /// absorbed into a fused opcode earlier in the stream. The verifier
    /// accepts Nop; the dispatcher silently skips it (advance pc by 1).
    /// B10 fix: previously the fusion engine installed Opcode::Invalid
    /// in subsequent slots, which the verifier rejects.
    /// Value 50 is reserved for Nop (between SetField=49 and SemanticMax).
    Nop                = 50,
    // Load/store
    LoadArg            = 1,
    LoadConst          = 2,
    LoadLocal          = 3,
    StoreLocal         = 4,
    // Property access
    GetProp            = 5,
    SetProp            = 6,
    // Arithmetic
    Add                = 7,
    Sub                = 8,
    Mul                = 9,
    Div                = 10,
    Mod                = 11,
    // Comparison
    Eq                 = 12,
    Lt                 = 13,
    Gt                 = 14,
    Le                 = 15,
    Ge                 = 16,
    Ne                 = 17,
    // Control flow
    Call               = 18,
    Jump               = 19,
    Branch             = 20,
    Return             = 21,
    // Object construction
    MakeObject         = 22,
    MakeClosure        = 23,
    // Iteration
    GetIter            = 24,
    Next               = 25,
    // Concurrency (DESIGN.md language features §3)
    Spawn              = 26,
    Await              = 27,
    SyncEnter          = 28,
    SyncExit           = 29,
    ChanSend            = 30,
    ChanRecv            = 31,
    // Exception handling
    Raise              = 32,
    TryBegin           = 33,
    TryEnd             = 34,
    // Pattern matching
    Match              = 35,
    // Resource management (DESIGN.md §4 language features)
    Using              = 36,  // `using` block entry
    Defer              = 37,
    // Misc
    Dup                = 38,
    Pop                = 39,
    IsNull             = 40,
    IsInt              = 41,
    IsFloat            = 42,
    IsStr              = 43,
    IsObject           = 44,
    Coerce             = 45,   // implicit morphing
    InjectTrait        = 46,
    RemoveTrait        = 47,
    GetField           = 48,   // direct slot access (skips shape check)
    SetField           = 49,
    // End of semantic range
    SemanticMax        = 127,

    // === Quickened opcodes (DESIGN.md §4.2) — range [128, 223] ===
    LoadLocalFast      = 128,
    GetPropMono        = 129,
    SetPropMono        = 130,
    AddIntFast         = 131,
    AddFloatFast       = 132,
    AddStringConcatFast = 133,
    SubIntFast         = 134,
    MulIntFast         = 135,
    DivIntFast         = 136,
    CallMono           = 137,
    CallDirectSmall    = 138,
    BranchTakenFast    = 139,
    BranchNotTakenFast = 140,
    NextShapeFast      = 141,
    ToIntFast          = 142,
    ToFloatFast        = 143,
    ToStrFast          = 144,
    ToBoolFast         = 145,
    EqIntFast          = 146,
    LtIntFast          = 147,
    GetPropPoly        = 148,
    SetPropPoly        = 149,
    QuickenedMax       = 223,

    // === Fused opcodes (DESIGN.md §4.3) — range [224, 255] ===
    AddIntRR           = 224,   // reg-reg add
    AddIntRC           = 225,   // reg-const add
    AddStoreLocal      = 226,
    GetPropAddIntConst = 227,
    GetPropCallMono    = 228,
    LoadAddStore       = 229,
    CallMonoReturn     = 230,
    IterNextBranch     = 231,
    GetAddSetMono      = 232,
    FusedMax           = 255,
};

/// Returns true if the opcode is a semantic (un-quickened) opcode.
[[nodiscard]] constexpr bool is_semantic(Opcode op) noexcept {
    return static_cast<uint8_t>(op) <= static_cast<uint8_t>(Opcode::SemanticMax);
}

/// Returns true if the opcode is a quickened opcode.
[[nodiscard]] constexpr bool is_quickened(Opcode op) noexcept {
    uint8_t v = static_cast<uint8_t>(op);
    return v >= 128 && v <= static_cast<uint8_t>(Opcode::QuickenedMax);
}

/// Returns true if the opcode is a fused opcode.
[[nodiscard]] constexpr bool is_fused(Opcode op) noexcept {
    uint8_t v = static_cast<uint8_t>(op);
    return v >= 224 && v <= static_cast<uint8_t>(Opcode::FusedMax);
}

/// Returns the semantic opcode that a quickened or fused opcode must
/// fall back to. For semantic opcodes, returns the input.
/// For unknown mappings, returns Opcode::Invalid.
[[nodiscard]] constexpr Opcode fallback_for(Opcode op) noexcept {
    switch (op) {
        // Already semantic.
        case Opcode::LoadArg: case Opcode::LoadConst: case Opcode::LoadLocal:
        case Opcode::StoreLocal: case Opcode::GetProp: case Opcode::SetProp:
        case Opcode::Add: case Opcode::Sub: case Opcode::Mul: case Opcode::Div:
        case Opcode::Mod: case Opcode::Eq: case Opcode::Lt: case Opcode::Gt:
        case Opcode::Le: case Opcode::Ge: case Opcode::Ne:
        case Opcode::Call: case Opcode::Jump: case Opcode::Branch:
        case Opcode::Return: case Opcode::MakeObject: case Opcode::MakeClosure:
        case Opcode::GetIter: case Opcode::Next:
        case Opcode::Spawn: case Opcode::Await: case Opcode::SyncEnter:
        case Opcode::SyncExit: case Opcode::ChanSend: case Opcode::ChanRecv:
        case Opcode::Raise: case Opcode::TryBegin: case Opcode::TryEnd:
        case Opcode::Match: case Opcode::Using: case Opcode::Defer:
        case Opcode::Dup: case Opcode::Pop: case Opcode::IsNull: case Opcode::IsInt:
        case Opcode::IsFloat: case Opcode::IsStr: case Opcode::IsObject:
        case Opcode::Coerce: case Opcode::InjectTrait: case Opcode::RemoveTrait:
        case Opcode::GetField: case Opcode::SetField:
            return op;
        // Quickened -> semantic.
        case Opcode::LoadLocalFast:      return Opcode::LoadLocal;
        case Opcode::GetPropMono:        return Opcode::GetProp;
        case Opcode::SetPropMono:        return Opcode::SetProp;
        case Opcode::AddIntFast:         return Opcode::Add;
        case Opcode::AddFloatFast:       return Opcode::Add;
        case Opcode::AddStringConcatFast:return Opcode::Add;
        case Opcode::SubIntFast:         return Opcode::Sub;
        case Opcode::MulIntFast:         return Opcode::Mul;
        case Opcode::DivIntFast:         return Opcode::Div;
        case Opcode::CallMono:           return Opcode::Call;
        case Opcode::CallDirectSmall:    return Opcode::Call;
        case Opcode::BranchTakenFast:    return Opcode::Branch;
        case Opcode::BranchNotTakenFast: return Opcode::Branch;
        case Opcode::NextShapeFast:      return Opcode::Next;
        case Opcode::ToIntFast:          return Opcode::Coerce;
        case Opcode::ToFloatFast:        return Opcode::Coerce;
        case Opcode::ToStrFast:          return Opcode::Coerce;
        case Opcode::ToBoolFast:         return Opcode::Coerce;
        case Opcode::EqIntFast:          return Opcode::Eq;
        case Opcode::LtIntFast:          return Opcode::Lt;
        case Opcode::GetPropPoly:        return Opcode::GetProp;
        case Opcode::SetPropPoly:        return Opcode::SetProp;
        // Fused -> semantic (picks the first subop's semantic op).
        case Opcode::AddIntRR:           return Opcode::Add;
        case Opcode::AddIntRC:           return Opcode::Add;
        case Opcode::AddStoreLocal:      return Opcode::Add;
        case Opcode::GetPropAddIntConst: return Opcode::GetProp;
        case Opcode::GetPropCallMono:     return Opcode::GetProp;
        case Opcode::LoadAddStore:        return Opcode::LoadLocal;
        case Opcode::CallMonoReturn:     return Opcode::Call;
        case Opcode::IterNextBranch:     return Opcode::Next;
        case Opcode::GetAddSetMono:      return Opcode::GetProp;
        default:                          return Opcode::Invalid;
    }
}

}  // namespace omni::bytecode
