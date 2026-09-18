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

// --- Index / id types (Rule 15, Rule 16) ---

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
constexpr BytecodePC INVALID_PC = 0xFFFFFFFF;

/// Bytecode site identifier (a module id + pc pair encoded as a single
/// 64-bit value for cheap hashing and comparison in IC tables).
using SiteId = uint64_t;

/// Register identifier inside an InterpFrame. Laws Part I: 256 virtual
/// registers per frame.
using RegId = uint8_t;
constexpr RegId INVALID_REG = 0xFF;

/// Region identifier. DESIGN.md §9: regions are the unit of invalidation
/// and partial-deopt rebuild.
using RegionId = uint32_t;
constexpr RegionId NULL_REGION = 0;

/// Tier identifier. DESIGN.md §6: T0 (interpreter) + T1/T2/T3/T4 (JIT).
using TierId = uint8_t;

// --- Capacity constants (Rule 23: no magic numbers) ---

/// Laws Part I §1 (Tier 0): 24-bit fixed-width instructions.
constexpr unsigned INSTRUCTION_WIDTH_BITS = 24;
constexpr unsigned INSTRUCTION_WIDTH_BYTES = 3;
static_assert(INSTRUCTION_WIDTH_BYTES * 8 == INSTRUCTION_WIDTH_BITS);

/// Laws Part I §1 (Tier 0): 256 virtual registers per frame.
constexpr unsigned FRAME_REGISTER_COUNT = 256;
static_assert(FRAME_REGISTER_COUNT == (1u << 8));

/// Maximum bytecode length per module, bounded so pc fits in uint32_t
/// and so a single module never exceeds the code-cache budget per function
/// (Rule 112). 16M instructions is generous but bounded.
constexpr unsigned MAX_BYTECODE_LENGTH = 1u << 24;

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

/// IC configuration constants (DESIGN.md §5.6).
/// A monomorphic IC has one entry. When it overflows the monomorphic slot
/// it transitions to polymorphic. Polymorphic ICs hold up to N entries
/// before going megamorphic.
constexpr unsigned IC_MONOMORPHIC_CAPACITY = 1;
constexpr unsigned IC_POLYMORPHIC_CAPACITY = 4;
/// Megamorphic ICs fall back to a generic dictionary lookup; no fixed cap.

/// Site profile heat threshold. Below this counter value the site is
/// considered cold and no quickening is attempted (DESIGN.md §5.4).
constexpr uint32_t SITE_HOT_THRESHOLD = 50;

/// Branch bias threshold for BRANCH_TAKEN_FAST quickening.
/// Stored as numerator/denominator over 1024 samples for fixed-point math.
constexpr uint32_t BRANCH_BIAS_NUM = 900;    // ~88%
constexpr uint32_t BRANCH_BIAS_DEN = 1024;
constexpr uint32_t BRANCH_BIAS_SAMPLE_MIN = 16;

/// Recursion / stack-depth limit. Tier 0 must respect Omni recursion
/// limits (Laws Rule 90). 1024 frames is a conservative default; the
/// runtime can lower this at startup based on actual stack size.
constexpr uint32_t DEFAULT_RECURSION_LIMIT = 1024;

/// Safepoint poll interval (in bytecode sites executed).
/// Rule 88: safepoint latency must be bounded.
constexpr uint32_t SAFEPONENT_POLL_INTERVAL = 1024;

}  // namespace omni::common
