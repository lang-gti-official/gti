#include "gti/driver/test_report.h"

#include <charconv>
#include <ostream>

namespace lang::driver {
namespace {

void writeString(std::ostream &out, std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  out.put('"');
  for (std::size_t i = 0; i < value.size();) {
    const auto c = static_cast<unsigned char>(value[i]);
    if (c < 0x80) {
      ++i;
      switch (c) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 0x20) {
          out << "\\u00";
          out.put(hex[c >> 4]);
          out.put(hex[c & 15]);
        } else {
          out.put(static_cast<char>(c));
        }
      }
      continue;
    }
    // Preserve valid UTF-8 verbatim. Replace each invalid byte with U+FFFD,
    // including overlong encodings, surrogates, and out-of-range scalars.
    const std::size_t count = c >= 0xc2 && c <= 0xdf ? 2 :
                              c >= 0xe0 && c <= 0xef ? 3 :
                              c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    bool valid = count != 0 && count <= value.size() - i;
    std::uint32_t scalar = count == 2 ? c & 0x1f : count == 3 ? c & 0x0f : c & 7;
    for (std::size_t j = 1; valid && j < count; ++j) {
      const auto continuation = static_cast<unsigned char>(value[i + j]);
      valid = continuation >= 0x80 && continuation <= 0xbf;
      scalar = (scalar << 6) | (continuation & 0x3f);
    }
    valid = valid && scalar >= (count == 2 ? 0x80U : count == 3 ? 0x800U : 0x10000U) &&
            scalar <= 0x10ffff && !(scalar >= 0xd800 && scalar <= 0xdfff);
    if (valid) {
      out.write(value.data() + i, static_cast<std::streamsize>(count));
      i += count;
    } else {
      out << "\\ufffd";
      ++i;
    }
  }
  out.put('"');
}

template <class Integer> void writeNumber(std::ostream &out, Integer value) {
  char buffer[32];
  const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
  out.write(buffer, static_cast<std::streamsize>(converted.ptr - buffer));
}

void beginEvent(std::ostream &out, std::string_view event) {
  out << "{\"schema_version\":1,\"event\":";
  writeString(out, event);
}

void identity(std::ostream &out, std::string_view target, std::string_view name) {
  out << ",\"target\":"; writeString(out, target);
  out << ",\"name\":"; writeString(out, name);
}

void optionalString(std::ostream &out, const std::optional<std::string> &value) {
  if (value) writeString(out, *value);
  else out << "null";
}

void finishEvent(std::ostream &out) { out << "}\n"; out.flush(); }

} // namespace

std::string_view testCaseStatusName(TestCaseStatus status) {
  switch (status) {
  case TestCaseStatus::Passed: return "passed";
  case TestCaseStatus::Failed: return "failed";
  case TestCaseStatus::Crashed: return "crashed";
  case TestCaseStatus::TimedOut: return "timed_out";
  case TestCaseStatus::RunnerError: return "runner_error";
  case TestCaseStatus::Skipped: return "skipped";
  }
  return "runner_error";
}

void writeTestInventoryJson(std::ostream &out, std::string_view target,
                            std::span<const TestSelection> tests) {
  beginEvent(out, "suite_discovered");
  out << ",\"target\":"; writeString(out, target);
  out << ",\"tests\":[";
  bool first = true;
  for (const auto &test : tests) {
    if (!first) out.put(',');
    first = false;
    out << "{\"name\":"; writeString(out, test.name);
    out << ",\"selected\":" << (test.selected ? "true" : "false");
    out << ",\"skip_reason\":";
    if (test.selected) out << "null";
    else writeString(out, test.skipReason);
    out.put('}');
  }
  out.put(']');
  finishEvent(out);
}

void writeTestStartedJson(std::ostream &out, std::string_view target,
                          std::string_view name) {
  beginEvent(out, "test_started");
  identity(out, target, name);
  finishEvent(out);
}

void writeTestFinishedJson(std::ostream &out, const TestCaseReport &report) {
  beginEvent(out, "test_finished");
  identity(out, report.target, report.name);
  out << ",\"status\":"; writeString(out, testCaseStatusName(report.status));
  out << ",\"exit_code\":"; writeNumber(out, report.exitCode);
  out << ",\"signal\":";
  if (report.signal) writeNumber(out, *report.signal);
  else out << "null";
  out << ",\"duration_ms\":"; writeNumber(out, report.durationMs);
  out << ",\"output\":"; writeString(out, report.output);
  out << ",\"diagnostic\":"; optionalString(out, report.diagnostic);
  finishEvent(out);
}

void writeTestSummaryJson(std::ostream &out, const TestRunSummary &summary) {
  beginEvent(out, "run_finished");
  out << ",\"discovered\":"; writeNumber(out, summary.discovered);
  out << ",\"selected\":"; writeNumber(out, summary.selected);
  out << ",\"started\":"; writeNumber(out, summary.started);
  out << ",\"completed\":"; writeNumber(out, summary.completed);
  out << ",\"passed\":"; writeNumber(out, summary.passed);
  out << ",\"failed\":"; writeNumber(out, summary.failed);
  out << ",\"crashed\":"; writeNumber(out, summary.crashed);
  out << ",\"timed_out\":"; writeNumber(out, summary.timedOut);
  out << ",\"runner_errors\":"; writeNumber(out, summary.runnerErrors);
  out << ",\"skipped\":"; writeNumber(out, summary.skipped);
  out << ",\"list_only\":" << (summary.listOnly ? "true" : "false");
  out << ",\"complete\":" << (summary.complete ? "true" : "false");
  out << ",\"exit_code\":"; writeNumber(out, summary.exitCode);
  out << ",\"diagnostic\":"; optionalString(out, summary.diagnostic);
  finishEvent(out);
}

} // namespace lang::driver
