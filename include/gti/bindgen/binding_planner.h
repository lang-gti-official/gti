#pragma once

#include "gti/bindgen/native_model.h"

#include <map>
#include <string>
#include <vector>

// The mapping-policy seam. The planner walks a native model and decides, for
// every declaration, either the exact GTI binding it becomes or a recorded
// skip with the reason — nothing is silently dropped. Policy lives here so
// future C++ mapping (namespaces, methods, renames, allow/deny lists)
// changes this phase only.

namespace lang::bindgen {

// One `name` or `prefix*` selector. Exact names and a single trailing
// wildcard are the whole grammar: an import policy has to be auditable, and
// a general expression language would make "what does this header export?"
// unanswerable without running the tool.
struct BindingNamePattern {
  std::string text;

  [[nodiscard]] bool matches(std::string_view name) const {
    if (!text.empty() && text.back() == '*') {
      return name.substr(0, text.size() - 1) ==
             std::string_view(text).substr(0, text.size() - 1);
    }
    return name == text;
  }
};

// The pointer-plus-count contract for one function result (C-07). C headers
// cannot state it, so the caller declares it and the planner validates the
// shape against the language's `[[c_array(count)]]` rule.
struct BindingArrayReturn {
  std::string function;
  std::string countParameter;
};

// Opt-in import policy (C-07). Every field narrows or renames what the
// bounded surface already admits; none of it widens the ABI.
struct BindingOptions {
  // Optional GTI namespace wrapped around the generated source names; the C
  // symbols stay global exactly as the language defines.
  std::string namespaceName;
  // When non-empty, only matching declarations are emitted. Non-matching
  // declarations still resolve uses exactly like out-of-policy system
  // declarations do.
  std::vector<BindingNamePattern> allow;
  // Matching declarations are never emitted; deny wins over allow.
  std::vector<BindingNamePattern> deny;
  // C name to GTI source name. Functions retain their native symbol through
  // `[[link_name("...")]]` when the two spellings differ.
  std::map<std::string, std::string> renames;
  // Functions whose result is borrowed array storage counted through an out
  // parameter.
  std::vector<BindingArrayReturn> arrayReturns;
};

enum class BindingItemKind {
  // [[c_abi]] struct with every field expressible.
  Record,
  // [[c_opaque]] handle: an incomplete record, or a complete record whose
  // layout the GTI ABI cannot express, degraded so pointer-based APIs
  // through it stay importable. `reason` is set for the degraded case.
  Opaque,
  // Alias to the target-compatible C integer, plus constexpr enumerators.
  Enum,
  // using Name = <spelling>; for an object typedef.
  Alias,
  // using Name = (params) -> result; for a function-pointer typedef, or a
  // deterministic alias the planner synthesized for an inline
  // function-pointer use (`synthesized` marks the latter).
  Callback,
  // constexpr <type> Name = value; for an imported macro constant.
  Constant,
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
  // Exact decimal spelling selected from the native signed/unsigned value.
  std::string value;
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
  bool recordIsUnion = false;
  std::vector<BindingField> fields;
  // Enum.
  std::vector<BindingConstant> constants;
  // Alias, enum-compatible integer, or the GTI scalar type of a Constant.
  std::string aliasSpelling;
  // Callback / Function.
  std::string resultSpelling;
  std::vector<BindingParameter> parameters;
  // Constant: `aliasSpelling` is its GTI type and the tagged value follows
  // the native model's evaluated form.
  NativeConstantKind constantKind = NativeConstantKind::Integer;
  std::int64_t constantValue = 0;
  double constantFloating = 0.0;
  std::string constantText;
  // Callback: the alias was synthesized for an inline function-pointer use
  // rather than read from a header typedef.
  bool synthesized = false;
  // Function: the validated `[[c_array(count)]]` count parameter, empty when
  // the function declares no array-return contract.
  std::string arrayCountParameter;
  // Function: the exact C link symbol when `name` is a different GTI source
  // spelling, empty when both names are identical.
  std::string linkName;
  // Opaque degradation note or skip reason.
  std::string reason;
};

struct BindingPlan {
  BindingOptions options;
  // Header declaration order; the writer preserves it.
  std::vector<BindingItem> items;
  // The model's consumed-file table (C-05): item and diagnostic origins
  // resolve their `file` ids here; 0 is the root header.
  std::vector<NativeSourceFile> sourceFiles;
  // Configuration entries that matched no declaration, or that named a
  // shape the policy refused. A typo in an opt-in list must be visible
  // rather than silently doing nothing.
  std::vector<std::string> policyProblems;
  [[nodiscard]] std::size_t importedCount() const;
  [[nodiscard]] std::size_t skippedCount() const;
};

[[nodiscard]] BindingPlan planBindings(const NativeModel &model,
                                       BindingOptions options);

} // namespace lang::bindgen
