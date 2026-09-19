// core/gc/gc_handle.cpp
//
// HandleTable and HandleScope implementation.

#include "core/gc/gc_handle.hpp"

namespace omni::gc {

std::atomic<uint32_t> HandleScope::next_scope_id_{1};

}  // namespace omni::gc
