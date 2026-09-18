// core/common/types.hpp
//
// Foundational type aliases and constants used across all Omni modules.
//
// Purpose:
//   Defines the index/id types mandated by Laws Rule 15 (index-based graph),
//   Rule 16 (interned symbols), and Rule 23 (no magic numbers).
//
// Invariants:
//   - All id types are fixed-width unsigned integers.
//   - NULL_* constants are zero (reserved sentinel).
//   - Capacity constants are documented constexpr values with rationale.
//
// Cross-references:
//   - DESIGN.md §3.2 (OmniShape: shape_id, shape_version, epoch)
//   - DESIGN.md §5.3 (SiteProfile: pc, shape_id, shape_version)
//   - LAWS.md Rule 15 (index-based graph, NodeId = uint32_t)
//   - LAWS.md Rule 16 (SymbolId = uint32_t)
//   - LAWS.md Rule 23 (no magic numbers)

#pragma once

#include <cstdint>

namespace omni::common {

// ============================================================================
// Section 1: Bit-width and structural constants
//
// These are defined first because other sections depend on them
// (e.g. BITS_PER_BYTE is used to compute sizeof-derived constants).
// They are not tuning knobs; they are structural invariants of the data
// formats and must not change without bumping the IR/ABI version
// (Rule 31).
// ============================================================================

/// Number of bits in one byte. Universal constant.
constexpr unsigned BITS_PER_BYTE = 8;

/// Bit position of bytes 0..3 within a 32-bit word. Used by
/// Instruction::operand_ab() and Instruction::raw() when packing/unpacking
/// 24-bit instructions from a byte stream.
constexpr unsigned BYTE_SHIFT_0 = 0;
constexpr unsigned BYTE_SHIFT_1 = BITS_PER_BYTE;
constexpr unsigned BYTE_SHIFT_2 = BITS_PER_BYTE * 2;
constexpr unsigned BYTE_SHIFT_3 = BITS_PER_BYTE * 3;

/// Laws Part I §1 (Tier 0): 24-bit fixed-width instructions.
constexpr unsigned INSTRUCTION_WIDTH_BITS = 24;
constexpr unsigned INSTRUCTION_WIDTH_BYTES = INSTRUCTION_WIDTH_BITS / BITS_PER_BYTE;
static_assert(INSTRUCTION_WIDTH_BYTES * BITS_PER_BYTE == INSTRUCTION_WIDTH_BITS);

/// Number of bits in a RegId. 256 registers per frame means RegId is 8 bits.
constexpr unsigned REG_ID_BITS = BITS_PER_BYTE;

/// Number of bits in a BytecodePC. Used to size MAX_BYTECODE_LENGTH.
constexpr unsigned BYTECODE_PC_BITS = 32;

/// Width of a BytecodePC sentinel "invalid" value.
constexpr uint32_t INVALID_PC_SENTINEL = 0xFFFFFFFFu;

/// Width of a RegId sentinel "invalid" value.
constexpr uint8_t INVALID_REG_SENTINEL = 0xFFu;

// ============================================================================
// Section 2: Index / id types (Rule 15, Rule 16)
// ============================================================================

/// Node identifier in the Sea of Nodes IR. Rule 15: never use raw pointers
/// for edges; all references go through this 32-bit index.
using NodeId = uint32_t;
constexpr NodeId NULL_NODE = 0;

/// Interned symbol identifier. Rule 16: all identifiers in IR and passes
/// must use SymbolId, not std::string or std::string_view.
using SymbolId = uint32_t;
constexpr SymbolId NULL_SYMBOL = 0;

/// Shape identifier. DESIGN.md §3.2: every OmniShape has a stable shape_id
/// that survives across versions of the same shape lineage.
using ShapeId = uint32_t;
constexpr ShapeId NULL_SHAPE = 0;

/// Shape version. DESIGN.md §3.2: bumps on every transition
/// (AddField, AddTrait, OverrideMethod, etc.).
using ShapeVersion = uint32_t;
constexpr ShapeVersion INITIAL_SHAPE_VERSION = 1;

/// Dependency epoch. DESIGN.md §3.2 + §10.1: monotonically increasing
/// counter per dependency class; bumped when invalidation occurs.
using Epoch = uint64_t;
constexpr Epoch INITIAL_EPOCH = 1;

/// Bytecode program counter. The interpreter pc is an index into the
/// instruction stream of the current BytecodeModule.
using BytecodePC = uint32_t;
/// Invalid pc sentinel. Stored as a named constant, not a magic number
/// (Rule 23). Callers must use INVALID_PC, not the raw value.
constexpr BytecodePC INVALID_PC = INVALID_PC_SENTINEL;

/// Bytecode site identifier (a module id + pc pair encoded as a single
/// 64-bit value for cheap hashing and comparison in IC tables).
using SiteId = uint64_t;

/// Register identifier inside an InterpFrame. Laws Part I: 256 virtual
/// registers per frame.
using RegId = uint8_t;
/// Invalid register sentinel. 255 is reserved.
constexpr RegId INVALID_REG = INVALID_REG_SENTINEL;

/// Region identifier. DESIGN.md §9: regions are the unit of invalidation
/// and partial-deopt rebuild.
using RegionId = uint32_t;
constexpr RegionId NULL_REGION = 0;

/// Tier identifier. DESIGN.md §6: T0 (interpreter) + T1/T2/T3/T4 (JIT).
using TierId = uint8_t;

// ============================================================================
// Section 3: Capacity constants (Rule 23: no magic numbers)
// ============================================================================

/// Laws Part I §1 (Tier 0): 256 virtual registers per frame.
constexpr unsigned FRAME_REGISTER_COUNT = 1u << REG_ID_BITS;
static_assert(FRAME_REGISTER_COUNT == 256);
static_assert(sizeof(RegId) * BITS_PER_BYTE == REG_ID_BITS);

/// Maximum bytecode length per module, bounded so pc fits in uint32_t
/// and so a single module never exceeds the code-cache budget per function
/// (Rule 112). 16M instructions is generous but bounded.
constexpr unsigned MAX_BYTECODE_LENGTH = 1u << INSTRUCTION_WIDTH_BITS;
static_assert(MAX_BYTECODE_LENGTH == (1u << 24));

/// Dispatch table size: one slot per possible 8-bit opcode value.
constexpr unsigned DISPATCH_TABLE_SIZE = 1u << REG_ID_BITS;
static_assert(DISPATCH_TABLE_SIZE == 256);

/// Proxy trap table size. ProxyPayload.traps is a fixed array of this
/// many TaggedValues: get, set, has, delete, call, iterate, next,
/// length, has_item, keys, values, entries, get_prototype, etc.
/// DESIGN.md §3.1 ProxyObject payload.
constexpr unsigned PROXY_TRAP_COUNT = 16;

/// GC map: number of bits per uint64 chunk. The register file has
/// FRAME_REGISTER_COUNT registers; we track GC-reference bits in
/// chunks of GC_MAP_BITS_PER_CHUNK bits.
/// FRAME_REGISTER_COUNT=256, GC_MAP_BITS_PER_CHUNK=64 => GC_MAP_CHUNKS=4.
constexpr unsigned GC_MAP_BITS_PER_CHUNK = 64;
constexpr unsigned GC_MAP_CHUNKS = (FRAME_REGISTER_COUNT + GC_MAP_BITS_PER_CHUNK - 1)
                                   / GC_MAP_BITS_PER_CHUNK;
static_assert(GC_MAP_CHUNKS == 4);

/// Number of bits per uint64. Used in bit-shift expressions where a
/// single bit needs to be set/cleared/tested.
constexpr uint64_t ONE_BIT = 1ull;

// ============================================================================
// Section 4: IC slot packing constants (DESIGN.md §5.6)
//
// The 64-bit IC slot packs:
//   bits [0, 32)   shape_id
//   bits [32, 48)  shape_version (low 16 bits; full version recoverable
//                  via the shape's epoch)
//   bits [48, 64)  field offset (byte offset or slot index)
// ============================================================================

constexpr unsigned IC_SLOT_SHAPE_ID_BITS = 32;
constexpr unsigned IC_SLOT_VERSION_BITS = 16;
constexpr unsigned IC_SLOT_OFFSET_BITS = 16;

constexpr unsigned IC_SLOT_SHAPE_ID_SHIFT = 0;
constexpr unsigned IC_SLOT_VERSION_SHIFT = IC_SLOT_SHAPE_ID_SHIFT + IC_SLOT_SHAPE_ID_BITS;
constexpr unsigned IC_SLOT_OFFSET_SHIFT = IC_SLOT_VERSION_SHIFT + IC_SLOT_VERSION_BITS;
static_assert(IC_SLOT_OFFSET_SHIFT + IC_SLOT_OFFSET_BITS == 64);

constexpr uint32_t IC_SLOT_SHAPE_ID_MASK =
    (IC_SLOT_SHAPE_ID_BITS == 32) ? ~uint32_t{0}
                                  : ((uint32_t{1} << IC_SLOT_SHAPE_ID_BITS) - 1u);
constexpr uint16_t IC_SLOT_VERSION_MASK =
    static_cast<uint16_t>((1u << IC_SLOT_VERSION_BITS) - 1u);

// ============================================================================
// Section 5: Tier promotion thresholds (DESIGN.md §6 tier ladder)
// ============================================================================

/// T1 promotion threshold (DESIGN.md tier ladder).
constexpr uint32_t T1_PROMOTION_THRESHOLD = 1000;

/// Monomorphic ratio required to promote T1->T2 (DESIGN.md tier ladder).
/// Stored as numerator/denominator so comparisons are integer.
constexpr uint32_t T2_PROMOTION_MONO_NUM = 80;   // 80%
constexpr uint32_t T2_PROMOTION_MONO_DEN = 100;

/// Guard failure rate ceiling for T1->T2 promotion.
constexpr uint32_t T2_PROMOTION_GUARD_FAIL_NUM = 5;   // 5%
constexpr uint32_t T2_PROMOTION_GUARD_FAIL_DEN = 100;

/// Maximum full deopts per method before T2 blacklists the method.
constexpr uint32_t T2_DEOPT_BLACKLIST_THRESHOLD = 3;

/// Tier-0 site-disable threshold. Number of failures (IC miss, guard
/// failure) at a single site before the site is marked Disabled and
/// stops quickening. Distinct from T2_DEOPT_BLACKLIST_THRESHOLD because
/// the semantics differ: Tier-0 disable is per-site; Tier-2 blacklist
/// is per-method. (B28 fix.)
constexpr uint32_t T0_SITE_DISABLE_FAILURE_THRESHOLD = 3;

/// Region stability required for T2->T3 promotion (DESIGN.md tier ladder).
/// Stored as numerator/denominator.
constexpr uint32_t T3_PROMOTION_STABILITY_NUM = 90;   // 0.9
constexpr uint32_t T3_PROMOTION_STABILITY_DEN = 100;
constexpr uint64_t T3_PROMOTION_LOOP_ITER_THRESHOLD = 10000;

/// Partial-deopt budget per region before T3 demotes the region.
constexpr uint32_t T3_PARTIAL_DEOPT_BUDGET = 2;
constexpr uint32_t T3_PARTIAL_DEOPT_BACKOFF_FACTOR = 2;

/// T3->T4 entrance criteria.
constexpr uint64_t T4_PROMOTION_UPTIME_NS = 60'000'000'000ull;  // 60 seconds
constexpr uint32_t T4_PROMOTION_SHAPE_CHURN_NUM = 1;   // 1%
constexpr uint32_t T4_PROMOTION_SHAPE_CHURN_DEN = 100;
constexpr uint32_t T4_PROMOTION_CLUSTER_HOTNESS_NUM = 90;   // 90%
constexpr uint32_t T4_PROMOTION_CLUSTER_HOTNESS_DEN = 100;

// ============================================================================
// Section 6: Inline cache configuration (DESIGN.md §5.6)
// ============================================================================

/// A monomorphic IC has one entry. When it overflows the monomorphic slot
/// it transitions to polymorphic.
constexpr unsigned IC_MONOMORPHIC_CAPACITY = 1;

/// Polymorphic ICs hold up to N entries before going megamorphic.
constexpr unsigned IC_POLYMORPHIC_CAPACITY = 4;
/// Megamorphic ICs fall back to a generic dictionary lookup; no fixed cap.

// ============================================================================
// Section 7: Site-profile and quickening thresholds (DESIGN.md §5.3, §5.4)
// ============================================================================

/// Site profile heat threshold. Below this counter value the site is
/// considered cold and no quickening is attempted (DESIGN.md §5.4).
constexpr uint32_t SITE_HOT_THRESHOLD = 50;

/// Branch bias threshold for BRANCH_TAKEN_FAST quickening.
/// Stored as numerator over BIAS_RESOLUTION samples for fixed-point math.
constexpr unsigned BIAS_RESOLUTION_BITS = 10;
constexpr uint32_t BIAS_RESOLUTION = 1u << BIAS_RESOLUTION_BITS;  // 1024

/// Bias threshold (~88%) above which we quicken a branch as taken/not-taken.
constexpr uint32_t BRANCH_BIAS_NUM = 900;
constexpr uint32_t BRANCH_BIAS_DEN = BIAS_RESOLUTION;
constexpr uint32_t BRANCH_BIAS_SAMPLE_MIN = 16;

/// Fixed-point scaling for stability scores (DESIGN.md §3.8).
/// Scores are stored as Q8 fractions over STABILITY_Q_BASE.
constexpr unsigned STABILITY_Q_SHIFT = 8;
constexpr uint32_t STABILITY_Q_BASE = 1u << STABILITY_Q_SHIFT;  // 256
constexpr uint32_t STABILITY_Q_FULL = BIAS_RESOLUTION;          // 1024

// ============================================================================
// Section 8: Recursion and safepoint thresholds (Laws Rule 88, Rule 90)
// ============================================================================

/// Recursion / stack-depth limit. Tier 0 must respect Omni recursion
/// limits (Laws Rule 90). 1024 frames is a conservative default; the
/// runtime can lower this at startup based on actual stack size.
constexpr uint32_t DEFAULT_RECURSION_LIMIT = 1024;

/// Safepoint poll interval (in bytecode sites executed).
/// Rule 88: safepoint latency must be bounded.
constexpr uint32_t SAFEPONENT_POLL_INTERVAL = 1024;

}  // namespace omni::common
