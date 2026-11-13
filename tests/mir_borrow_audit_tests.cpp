// MIR re-hosts of the raw-pointer live-borrow audit and the cross-function
// persistent-storage borrow check (RFC 0004 phase 2), and their parity
// measurement against the semantic checks.
#include "../src/compiler/mir_borrow_audits.h"
#include "gti/frontend.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {
int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

const std::filesystem::path root =
    std::filesystem::path(__FILE__).parent_path().parent_path();

lang::FrontendResult analyze(const std::string &source, bool audit) {
  lang::FrontendOptions options;
  if (audit)
    options.rawPointerAudit = lang::RawPointerAuditMode::Warn;
  return lang::Frontend(options).analyze(root / "mir-borrow-audit-test.gti",
                                         source, {root / "stdlib/prelude.gti"},
                                         {}, {root / "stdlib"});
}

std::size_t rawFindings(const std::string &source) {
  const auto result = analyze(source, true);
  if (!result.mirValid) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 99;
  }
  return lang::auditMirRawAccesses(result.mir).size();
}

void rawAccesses() {
  expect(rawFindings(R"(
int main() {
  mut int value = 7;
  unsafe {
    auto pointer = &value;
    int& loan = value;
    *pointer = 9;
    return loan;
  }
})") == 1,
         "a raw write overlapping a live checked loan is a MIR finding");
  expect(rawFindings(R"(
int main() {
  mut int value = 7;
  unsafe {
    auto pointer = &value;
    int& loan = value;
    int before = loan;
    *pointer = before + 2;
    return value;
  }
})") == 0,
         "a loan MIR ended before the raw write is not live");
  expect(rawFindings(R"(
int main() {
  mut int value = 7;
  unsafe {
    auto pointer = &value;
    int& loan = value;
    int read = *pointer;
    return read + loan;
  }
})") == 0,
         "a raw read is compatible with a live shared loan");
  expect(rawFindings(R"(
struct Address { mut int* pointer = nullptr;  public: Address() = default; };
int main() {
  mut int value = 7;
  mut Address address{};
  Address* alias = &address;
  unsafe {
    alias->pointer = &value;
    int& checked = value;
    *address.pointer = 9;
    return checked;
  }
})") == 1,
         "provenance stored through a raw alias reaches the later access");
  expect(rawFindings(R"(
int main() {
  mut int values[2] = {1, 2};
  unsafe {
    auto pointer = &values[0];
    int& loan = values[1];
    *(pointer + 1) = 9;
    return loan;
  }
})") == 1,
         "pointer arithmetic widens an element origin to the array");
  expect(rawFindings(R"(
int main() {
  mut int value = 7;
  mut int other = 1;
  unsafe {
    mut int* pointer = &value;
    int& loan = value;
    pointer = &other;
    *pointer = 9;
    return loan;
  }
})") == 0,
         "reseating a raw pointer replaces its provenance");
  expect(rawFindings(R"(
unsafe void write_address(int* pointer) { unsafe { *pointer = 9; } }
unsafe void write_forwarded(int* pointer) { unsafe { write_address(pointer); } }
int main() {
  mut int owner = 7;
  int* pointer = &owner;
  int& held = owner;
  unsafe { write_forwarded(pointer); }
  return held;
})") == 1,
         "a callee's write through a raw formal is a caller finding, "
         "through forwarding calls");
  expect(rawFindings(R"(
mut int global = 7;
int* source() { return &global; }
int main() {
  auto pointer = source();
  int& checked = global;
  unsafe { *pointer = 9; }
  return checked;
})") == 1,
         "a callee's returned global address keeps its provenance");
  expect(rawFindings(R"(
int main() {
  mut int owner = 7;
  const int* pointer = &owner;
  auto read = [pointer]() -> int { unsafe { return *pointer; } };
  mut int& held = owner;
  int observed = read();
  held = 8;
  return observed + held;
})") == 1,
         "a closure's read through a captured raw pointer is a caller finding");
  expect(rawFindings(R"(
struct Cursor {
  mut int* target = nullptr;
public:
  Cursor() = default;
  void write() { unsafe { *this.target = 9; } }
};
int main() {
  mut int owner = 7;
  mut Cursor cursor{};
  unsafe { cursor.target = &owner; }
  int& held = owner;
  cursor.write();
  return held;
})") == 1,
         "a method's write through a receiver-held raw pointer is a caller "
         "finding");
}

void persistentStorageParity() {
  const auto log =
      std::filesystem::temp_directory_path() /
      ("gti-mir-borrow-audit-" + std::to_string(::getpid()) + ".log");
  std::filesystem::remove(log);
  ::setenv("GTI_PARITY_RAW_AUDIT", log.c_str(), 1);
  const auto rejected = analyze(R"(
mut int global = 1;
void bump() { global = global + 1; }
int main() {
  int& borrowed = global;
  bump();
  return borrowed;
}
)",
                                false);
  ::unsetenv("GTI_PARITY_RAW_AUDIT");
  std::ifstream input(log);
  std::stringstream text;
  text << input.rdbuf();
  std::filesystem::remove(log);
  expect(text.str().find("PARITY storage match") != std::string::npos,
         "MIR rejects the invocation semantics rejects:\n" + text.str());
  expect(!rejected.semanticValid && rejected.mir.functionInstances().empty(),
         "measurement restores the semantic rejection");
  const auto accepted = analyze(R"(
mut int global = 1;
void bump() { global = global + 1; }
int main() {
  int& borrowed = global;
  int copy = borrowed;
  bump();
  return copy;
}
)",
                                false);
  expect(accepted.mirValid &&
             lang::auditMirPersistentStorageBorrows(accepted.mir).empty(),
         "an ended loan admits the persistent-storage access");
}
} // namespace

int main() {
  rawAccesses();
  persistentStorageParity();
  return failures == 0 ? 0 : 1;
}
