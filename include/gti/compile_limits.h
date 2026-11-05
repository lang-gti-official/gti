#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace lang {

// Per-request compile-time resource limits: implementation quantities in the
// sense of C++ [implimits]. A limit bounds one request (one constexpr binding,
// static assertion, array extent, value-generic argument, compile-time branch
// condition, immediate invocation, macro invocation or `constexpr for`
// statement) and never accumulates across a module or compilation. The
// language reference guarantees each limit's minimum; programs that stay
// within every minimum are portable, and the compiler's defaults may exceed it.
//
// One value of this type configures a whole compilation: the command line
// overrides the root package's `[limits]` table, which overrides these
// defaults. Every evaluation path (semantic analysis, the staged MIR constant
// evaluator, the compatibility interpreter, macro and `constexpr for`
// expansion, and editor analysis) reads the same value, and it participates
// in every cache and build identity whose result it can change.
struct CompileLimits {
  // Constant-evaluation steps per request (Clang -fconstexpr-steps).
  std::size_t constexprSteps = 1048576;
  // Nested constant-evaluation calls, including staged evaluation, per
  // request (Clang -fconstexpr-depth; C++ [implimits] recursive constexpr
  // invocations).
  std::size_t constexprDepth = 512;
  // Nested macro invocations (the analogue of Clang -ftemplate-depth; C++
  // [implimits] recursively nested template instantiations).
  std::size_t macroDepth = 1024;
  // Macro bodies expanded by one outermost written macro invocation,
  // including every nested expansion it causes. Separate invocations never
  // share this budget.
  std::size_t macroExpansions = 8192;
  // Procedural syntax resources shared by one outer expansion and its nested
  // invocations. Discarded construction still consumes the allocation budget.
  std::size_t syntaxNodes = 65536;
  std::size_t syntaxBytes = 1048576;
  std::size_t syntaxTokens = 65536;
  std::size_t syntaxDepth = 256;
  // Elements of one `constexpr for` statement.
  std::size_t constexprForElements = 1024;
  // `constexpr for` body copies expanded per declaration.
  std::size_t constexprForCopies = 4096;

  friend bool operator==(const CompileLimits &,
                         const CompileLimits &) = default;
};

// The configurable surface of one limit. The command-line flag is "--" + key;
// the manifest setting is `key = <n>` in the root package's `[limits]` table.
struct CompileLimitInfo {
  std::string_view key;
  std::size_t CompileLimits::*field = nullptr;
  // The portable floor the language reference guarantees.
  std::size_t guaranteedMinimum = 1;
  // The largest accepted setting; larger values are rejected rather than
  // clamped, so a configuration never silently means something else.
  std::size_t maximum = 1;
  // One-line description for command-line help.
  std::string_view summary;
};

// Every configurable limit, in presentation order.
[[nodiscard]] std::span<const CompileLimitInfo> compileLimitTable();
[[nodiscard]] const CompileLimitInfo *findCompileLimit(std::string_view key);
[[nodiscard]] const CompileLimitInfo &
compileLimitInfo(std::size_t CompileLimits::*field);

// A setting is a decimal integer in [1, maximum]. On failure the result is
// empty and `error` explains why, without naming where the value came from.
[[nodiscard]] std::optional<std::size_t>
parseCompileLimitValue(const CompileLimitInfo &limit, std::string_view text,
                       std::string &error);
[[nodiscard]] std::optional<std::size_t>
validateCompileLimitValue(const CompileLimitInfo &limit, long long value,
                          std::string &error);

// The diagnostic hint shared by every exhaustion report: names the flag, the
// manifest key and the limit in force.
[[nodiscard]] std::string compileLimitHint(const CompileLimitInfo &limit,
                                           std::size_t current);

// Deterministic spelling of every limit, for cache, model and build
// identities. Equal identities denote equal limits.
[[nodiscard]] std::string compileLimitsIdentity(const CompileLimits &limits);

// Explicit settings from one configuration source (the command line or a
// manifest). Unset limits inherit from the next source.
class CompileLimitOverrides final {
public:
  void set(const CompileLimitInfo &limit, std::size_t value);
  [[nodiscard]] std::optional<std::size_t>
  find(const CompileLimitInfo &limit) const;
  [[nodiscard]] bool empty() const { return values.empty(); }
  // Applies these settings over `limits`.
  void applyTo(CompileLimits &limits) const;

  friend bool operator==(const CompileLimitOverrides &,
                         const CompileLimitOverrides &) = default;

private:
  std::map<std::string_view, std::size_t> values;
};

// Command line over manifest over defaults.
[[nodiscard]] CompileLimits
resolveCompileLimits(const CompileLimitOverrides &manifest,
                     const CompileLimitOverrides &commandLine);

} // namespace lang
