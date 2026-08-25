#pragma once

#include "gti/bindgen/native_model.h"

#include <string>
#include <vector>

// The mapping-policy seam. The planner walks a native model and decides, for
// every declaration, either the exact GTI binding it becomes or a recorded
// skip with the reason — nothing is silently dropped. Policy lives here so
// future C++ mapping (namespaces, methods, renames, allow/deny lists)
// changes this phase only.

namespace lang::bindgen {

struct BindingOptions {
  // Optional GTI namespace wrapped around the generated source names; the C
  // symbols stay global exactly as the language defines.
  std::string namespaceName;
};

enum class BindingItemKind {
  // [[c_abi]] struct with every field expressible.
  Record,
  // [[c_opaque]] handle: an incomplete record, or a complete record whose
  // layout the GTI ABI cannot express, degraded so pointer-based APIs
  // through it stay importable. `reason` is set for the degraded case.
  Opaque,
  // using Name = int32_t; plus constexpr enumerator constants.
  Enum,
  // using Name = <spelling>; for an object typedef.
  Alias,
  // using Name = (params) -> result; for a function-pointer typedef.
  Callback,
  // One extern "C" declaration.
  Function,
  // Anything the bounded surface cannot express, with its reason.
  Skipped,
};

struct BindingParameter {
  std::string name;
  std::string spelling;
};

struct BindingConstant {
  std::string name;
  std::int64_t value = 0;
};

struct BindingField {
  std::string name;
  std::string spelling;
};

struct BindingItem {
  BindingItemKind kind = BindingItemKind::Skipped;
  std::string name;
  SourceOrigin origin;
  // Record.
  std::vector<BindingField> fields;
  // Enum.
  std::vector<BindingConstant> constants;
  // Alias.
  std::string aliasSpelling;
  // Callback / Function.
  std::string resultSpelling;
  std::vector<BindingParameter> parameters;
  // Opaque degradation note or skip reason.
  std::string reason;
};

struct BindingPlan {
  BindingOptions options;
  // Header declaration order; the writer preserves it.
  std::vector<BindingItem> items;
  [[nodiscard]] std::size_t importedCount() const;
  [[nodiscard]] std::size_t skippedCount() const;
};

[[nodiscard]] BindingPlan planBindings(const NativeModel &model,
                                       BindingOptions options);

} // namespace lang::bindgen
