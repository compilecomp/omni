// core/interpreter/speculative_arithmetic.cpp
//
// Out-of-line implementation of spec_str_concat (the only speculative
// arithmetic function that allocates). All int/float functions are
// defined inline in the header so the compiler can inline them into
// the quickened handlers.

#include "core/interpreter/speculative_arithmetic.hpp"

#include <string>

namespace omni::interpreter {

common::Result<object_model::TaggedValue>
spec_str_concat(object_model::TaggedValue a, object_model::TaggedValue b) noexcept {
    if (!a.is_str() || !b.is_str()) [[unlikely]] return detail::fallback();
    std::string_view sa = common::resolve_symbol(a.as_str());
    std::string_view sb = common::resolve_symbol(b.as_str());
    std::string combined;
    combined.reserve(sa.size() + sb.size());
    combined.append(sa);
    combined.append(sb);
    auto r = common::intern_symbol(combined);
    if (!r.has_value()) [[unlikely]] return std::unexpected(r.error());
    return object_model::TaggedValue::make_str(*r);
}

}  // namespace omni::interpreter
