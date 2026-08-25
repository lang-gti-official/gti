#pragma once

#include "gti/bindgen/binding_planner.h"

#include <string>
#include <string_view>

// Deterministic GTI source emission: a pure function from the binding plan
// to text. Identical plans produce byte-identical output. The generated
// source has no special language status — the frontend's own native-interop
// validation remains the ABI authority over it.

namespace lang::bindgen {

[[nodiscard]] std::string writeGtiBindings(const BindingPlan &plan,
                                           std::string_view headerName);

} // namespace lang::bindgen
