#include "gti/compile_limits.h"

#include <array>
#include <charconv>
#include <stdexcept>

namespace lang {
namespace {

// Each maximum is a measured safety bound, not a performance promise: the
// counters are 64-bit, the evaluator's storage is bounded independently of
// its step count, and nested work runs on fresh native stack segments
// (support.h), so the maxima bound time and memory rather than stack use.
constexpr std::array<CompileLimitInfo, 6> limits{{
    {.key = "constexpr-steps",
     .field = &CompileLimits::constexprSteps,
     .guaranteedMinimum = 1048576,
     .maximum = 4294967295ULL,
     .summary = "Constant-evaluation steps per request"},
    {.key = "constexpr-depth",
     .field = &CompileLimits::constexprDepth,
     .guaranteedMinimum = 512,
     .maximum = 16384,
     .summary = "Nested constant-evaluation calls per request"},
    {.key = "macro-depth",
     .field = &CompileLimits::macroDepth,
     .guaranteedMinimum = 1024,
     .maximum = 4096,
     .summary = "Nested macro invocations"},
    {.key = "macro-expansions",
     .field = &CompileLimits::macroExpansions,
     .guaranteedMinimum = 4096,
     .maximum = 32768,
     .summary = "Macro bodies expanded by one outermost macro invocation"},
    {.key = "constexpr-for-elements",
     .field = &CompileLimits::constexprForElements,
     .guaranteedMinimum = 1024,
     .maximum = 16384,
     .summary = "Elements of one 'constexpr for' statement"},
    {.key = "constexpr-for-copies",
     .field = &CompileLimits::constexprForCopies,
     .guaranteedMinimum = 4096,
     .maximum = 16384,
     .summary = "'constexpr for' body copies per declaration"},
}};

} // namespace

std::span<const CompileLimitInfo> compileLimitTable() { return limits; }

const CompileLimitInfo *findCompileLimit(std::string_view key) {
  for (const CompileLimitInfo &limit : limits)
    if (limit.key == key)
      return &limit;
  return nullptr;
}

const CompileLimitInfo &compileLimitInfo(std::size_t CompileLimits::*field) {
  for (const CompileLimitInfo &limit : limits)
    if (limit.field == field)
      return limit;
  throw std::logic_error("unregistered compile limit");
}

std::optional<std::size_t>
validateCompileLimitValue(const CompileLimitInfo &limit, long long value,
                          std::string &error) {
  if (value < 1 || static_cast<unsigned long long>(value) > limit.maximum) {
    error = "'" + std::string(limit.key) + "' must be an integer from 1 to " +
            std::to_string(limit.maximum) + "; got " + std::to_string(value) +
            ".";
    return std::nullopt;
  }
  return static_cast<std::size_t>(value);
}

std::optional<std::size_t> parseCompileLimitValue(const CompileLimitInfo &limit,
                                                  std::string_view text,
                                                  std::string &error) {
  unsigned long long value = 0;
  const auto [end, status] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || status == std::errc::invalid_argument ||
      end != text.data() + text.size()) {
    error = "'" + std::string(limit.key) +
            "' must be a positive decimal integer; got '" + std::string(text) +
            "'.";
    return std::nullopt;
  }
  if (status == std::errc::result_out_of_range || value < 1 ||
      value > limit.maximum) {
    error = "'" + std::string(limit.key) + "' must be an integer from 1 to " +
            std::to_string(limit.maximum) + "; got " + std::string(text) + ".";
    return std::nullopt;
  }
  return static_cast<std::size_t>(value);
}

std::string compileLimitHint(const CompileLimitInfo &limit,
                             std::size_t current) {
  return "The limit is " + std::to_string(current) + "; raise it with '--" +
         std::string(limit.key) + " <n>' or with '" + std::string(limit.key) +
         " = <n>' in the [limits] table of the root package's gti.toml.";
}

std::string compileLimitsIdentity(const CompileLimits &values) {
  std::string identity;
  for (const CompileLimitInfo &limit : limits) {
    if (!identity.empty())
      identity += ';';
    identity += limit.key;
    identity += '=';
    identity += std::to_string(values.*limit.field);
  }
  return identity;
}

void CompileLimitOverrides::set(const CompileLimitInfo &limit,
                                std::size_t value) {
  values[limit.key] = value;
}

std::optional<std::size_t>
CompileLimitOverrides::find(const CompileLimitInfo &limit) const {
  const auto found = values.find(limit.key);
  if (found == values.end())
    return std::nullopt;
  return found->second;
}

void CompileLimitOverrides::applyTo(CompileLimits &target) const {
  for (const CompileLimitInfo &limit : limits)
    if (const auto value = find(limit))
      target.*limit.field = *value;
}

CompileLimits resolveCompileLimits(const CompileLimitOverrides &manifest,
                                   const CompileLimitOverrides &commandLine) {
  CompileLimits resolved;
  manifest.applyTo(resolved);
  commandLine.applyTo(resolved);
  return resolved;
}

} // namespace lang
