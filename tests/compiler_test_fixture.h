#pragma once

#include "gti/ast_printer.h"
#include "gti/backend.h"
#include "gti/binary_float.h"
#include "gti/cpp_backend.h"
#include "gti/executable_path.h"
#include "gti/format_config.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/language_queries.h"
#include "gti/lexer.h"
#include "gti/lowered_program_printer.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"
#include "gti/optimizer.h"
#include "gti/parser.h"
#include "gti/semantic_analyzer.h"
#include "gti/source_loader.h"
#include "gti/standard_library.h"
#include "gti/support.h"

#include "cpp_backend_test_support.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace {

int failures = 0;

[[maybe_unused]] void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

[[maybe_unused]] bool containsMangledFunction(std::string_view text,
                                              std::string_view prefix,
                                              std::string_view sourceName,
                                              std::string_view suffix) {
  constexpr std::string_view legacyMarker = "__gti_fn_";
  constexpr std::string_view concreteMarker = "__gti_inst_";
  std::size_t search = 0;
  while (true) {
    const std::size_t legacy = text.find(legacyMarker, search);
    const std::size_t concrete = text.find(concreteMarker, search);
    const bool concreteName =
        concrete != std::string_view::npos &&
        (legacy == std::string_view::npos || concrete < legacy);
    const std::size_t function = concreteName ? concrete : legacy;
    if (function == std::string_view::npos) {
      return false;
    }
    const std::string_view marker =
        concreteName ? concreteMarker : legacyMarker;
    search = function + marker.size();
    if (function < prefix.size() ||
        text.substr(function - prefix.size(), prefix.size()) != prefix) {
      continue;
    }
    std::size_t tail = search;
    if (concreteName) {
      while (tail < text.size() && tail - search < 64 &&
             ((text[tail] >= '0' && text[tail] <= '9') ||
              (text[tail] >= 'a' && text[tail] <= 'f'))) {
        ++tail;
      }
      if (tail - search != 64) {
        continue;
      }
    } else {
      while (tail < text.size() && text[tail] >= '0' && text[tail] <= '9') {
        ++tail;
      }
      if (tail == search) {
        continue;
      }
    }
    if (tail >= text.size() || text[tail] != '_') {
      continue;
    }
    ++tail;
    if (text.substr(tail, sourceName.size()) != sourceName) {
      continue;
    }
    tail += sourceName.size();
    if (text.substr(tail, suffix.size()) == suffix) {
      return true;
    }
  }
}

[[maybe_unused]] std::unordered_set<std::string>
concreteBackendSymbols(std::string_view text, std::string_view marker,
                       std::string_view sourceName) {
  std::unordered_set<std::string> result;
  std::size_t search = 0;
  while ((search = text.find(marker, search)) != std::string_view::npos) {
    const std::size_t digest = search + marker.size();
    std::size_t tail = digest;
    while (tail < text.size() && tail - digest < 64 &&
           ((text[tail] >= '0' && text[tail] <= '9') ||
            (text[tail] >= 'a' && text[tail] <= 'f'))) {
      ++tail;
    }
    if (tail - digest == 64 && tail < text.size() && text[tail] == '_' &&
        text.substr(tail + 1, sourceName.size()) == sourceName) {
      const std::size_t end = tail + 1 + sourceName.size();
      result.emplace(text.substr(search, end - search));
    }
    search = digest;
  }
  return result;
}

[[maybe_unused]] std::string
emitAnalyzedSource(std::string_view source,
                   lang::TargetInfo target = lang::TargetInfo::host(),
                   lang::CppStandard standard = lang::CppStandard::Cpp23) {
  const lang::FrontendResult frontend =
      lang::Frontend({.target = target})
          .analyze("compiler-emission-fixture.gti", std::string(source));
  expect(frontend.canGenerateCode(),
         "a semantically valid emission fixture should complete MIR "
         "lowering");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected emission-fixture diagnostic: "
                << diagnostic.message << '\n';
    }
    return {};
  }
  return gti_test::emitCppText(frontend, standard, std::move(target));
}

[[maybe_unused]] bool hasDiagnostic(const lang::SemanticVisitor &semantic,
                                    const std::string &text) {
  for (const lang::SemanticDiagnostic &diagnostic : semantic.errors()) {
    if (diagnostic.message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

[[maybe_unused]] bool
hasDiagnostic(const std::vector<lang::Diagnostic> &diagnostics,
              const std::string &text) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

[[maybe_unused]] bool
hasDiagnosticCode(const std::vector<lang::Diagnostic> &diagnostics,
                  const std::string &code) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

[[maybe_unused]] const lang::Diagnostic *
findDiagnosticByCode(const std::vector<lang::Diagnostic> &diagnostics,
                     const std::string &code) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.code == code) {
      return &diagnostic;
    }
  }
  return nullptr;
}

[[maybe_unused]] bool
hasDiagnosticAt(const std::vector<lang::Diagnostic> &diagnostics,
                std::string_view code, std::string_view source,
                std::size_t start, std::string_view reason) {
  const bool matched =
      std::any_of(diagnostics.begin(), diagnostics.end(),
                  [&](const lang::Diagnostic &diagnostic) {
                    const std::string_view actualSource =
                        diagnostic.primary.source;
                    const std::string actualFile =
                        std::filesystem::path(diagnostic.primary.source)
                            .filename()
                            .string();
                    return diagnostic.code == code &&
                           (actualSource == source || actualFile == source) &&
                           diagnostic.primary.start == start &&
                           diagnostic.message.find(reason) != std::string::npos;
                  });
  if (!matched) {
    std::cerr << "Expected diagnostic " << code << " at " << source << ':'
              << start << " containing '" << reason << "'.\n";
    for (const lang::Diagnostic &diagnostic : diagnostics) {
      std::cerr << "  found " << diagnostic.code << " at "
                << diagnostic.primary.source << ':' << diagnostic.primary.start
                << ": " << diagnostic.message << '\n';
    }
  }
  return matched;
}

[[maybe_unused]] bool
hasMirVerificationError(const lang::MirVerificationResult &result,
                        std::string_view reason) {
  return std::any_of(result.errors.begin(), result.errors.end(),
                     [&](const lang::MirVerificationError &error) {
                       return error.message.find(reason) != std::string::npos;
                     });
}

[[maybe_unused]] bool
hasRelatedDiagnostic(const std::vector<lang::Diagnostic> &diagnostics,
                     const std::string &text) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    for (const lang::RelatedDiagnostic &related : diagnostic.related) {
      if (related.message.find(text) != std::string::npos) {
        return true;
      }
    }
  }
  return false;
}

[[maybe_unused]] bool
hasDiagnosticHint(const std::vector<lang::Diagnostic> &diagnostics,
                  const std::string &text) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    for (const std::string &hint : diagnostic.hints) {
      if (hint.find(text) != std::string::npos) {
        return true;
      }
    }
  }
  return false;
}

[[maybe_unused]] std::size_t
countDiagnosticCode(const lang::SemanticVisitor &semantic,
                    const std::string &code) {
  std::size_t count = 0;
  for (const lang::SemanticDiagnostic &diagnostic : semantic.errors()) {
    if (diagnostic.code == code) {
      ++count;
    }
  }
  return count;
}

[[maybe_unused]] std::size_t
countDiagnosticCode(const std::vector<lang::Diagnostic> &diagnostics,
                    const std::string &code) {
  std::size_t count = 0;
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.code == code) {
      ++count;
    }
  }
  return count;
}

[[maybe_unused]] std::size_t
countDiagnosticContaining(const std::vector<lang::Diagnostic> &diagnostics,
                          const std::string &text) {
  return static_cast<std::size_t>(
      std::count_if(diagnostics.begin(), diagnostics.end(),
                    [&](const lang::Diagnostic &item) {
                      return item.message.find(text) != std::string::npos;
                    }));
}

[[maybe_unused]] std::filesystem::path standardLibraryPrelude() {
  return std::filesystem::path(__FILE__).parent_path().parent_path() /
         "stdlib/prelude.gti";
}

[[maybe_unused]] std::filesystem::path standardLibraryRoot() {
  return standardLibraryPrelude().parent_path();
}

[[maybe_unused]] lang::FrontendResult
analyzeTrustedPreludeFixture(const std::string &name, const std::string &source,
                             lang::FrontendOptions options = {}) {
  const std::filesystem::path entry =
      std::filesystem::temp_directory_path() / ("gti-trusted-" + name);
  const std::string entryKey =
      std::filesystem::weakly_canonical(entry).string();
  return lang::Frontend(std::move(options))
      .analyze(entry, source, {standardLibraryPrelude(), entry},
               {{entryKey, source}}, {standardLibraryRoot()});
}

[[maybe_unused]] lang::FrontendResult
analyzeTrustedMemoryFixture(const std::string &name, const std::string &source,
                            lang::FrontendOptions options = {}) {
  return analyzeTrustedPreludeFixture(
      name, std::string("#include <std/memory>\n") + source,
      std::move(options));
}

[[maybe_unused]] lang::FrontendResult
analyzeTrustedStandalonePreludeFixture(const std::string &name,
                                       const std::string &source,
                                       lang::FrontendOptions options = {}) {
  const std::filesystem::path entry = std::filesystem::temp_directory_path() /
                                      ("gti-trusted-standalone-" + name);
  const std::string entryKey =
      std::filesystem::weakly_canonical(entry).string();
  return lang::Frontend(std::move(options))
      .analyze(entry, source, {entry}, {{entryKey, source}});
}

[[maybe_unused]] bool hasLoanEndpoint(const lang::SemanticLoanInfo *loan,
                                      lang::SemanticLoanEndKind kind,
                                      const lang::Stmt *statement) {
  return loan != nullptr &&
         std::any_of(loan->endpoints.begin(), loan->endpoints.end(),
                     [&](const lang::SemanticLoanEndpoint &endpoint) {
                       return endpoint.kind == kind &&
                              endpoint.statement == statement;
                     });
}

[[maybe_unused]] const lang::FunctionDecl *
findTopLevelFunction(const lang::Program &program, const std::string &name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *function =
        dynamic_cast<const lang::FunctionDecl *>(declaration.get());
    if (function != nullptr && function->name().lexeme == name) {
      return function;
    }
  }
  return nullptr;
}

[[maybe_unused]] const lang::ClassDecl *
findTopLevelClass(const lang::Program &program, const std::string &name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *classDecl =
        dynamic_cast<const lang::ClassDecl *>(declaration.get());
    if (classDecl != nullptr && classDecl->name().lexeme == name) {
      return classDecl;
    }
  }
  return nullptr;
}

[[maybe_unused]] const lang::ClassDecl *
findClassRecursive(const lang::StmtList &statements, const std::string &name) {
  for (const lang::StmtPtr &statement : statements) {
    const auto *classDecl =
        dynamic_cast<const lang::ClassDecl *>(statement.get());
    if (classDecl != nullptr && classDecl->name().lexeme == name) {
      return classDecl;
    }
    const auto *namespaceDecl =
        dynamic_cast<const lang::NamespaceDecl *>(statement.get());
    if (namespaceDecl != nullptr) {
      if (const lang::ClassDecl *nested =
              findClassRecursive(namespaceDecl->declarations(), name)) {
        return nested;
      }
    }
  }
  return nullptr;
}

[[maybe_unused]] const lang::VariableDecl *
findDirectLocal(const lang::FunctionDecl *function, const std::string &name) {
  if (function == nullptr || !function->body()) {
    return nullptr;
  }
  for (const lang::StmtPtr &statement : function->body()->statements()) {
    const auto *variable =
        dynamic_cast<const lang::VariableDecl *>(statement.get());
    if (variable != nullptr && variable->name().lexeme == name) {
      return variable;
    }
  }
  return nullptr;
}

[[maybe_unused]] std::string nonNullReferenceSource() {
  return R"(
struct Counter {
public:
  Counter() = default;
  mut int value = 0;
  void bump() mut { this.value += 1; }
};

int read(Counter& counter) { return counter.value; }
void increment(mut Counter& counter) { counter.bump(); }

int main() {
  mut Counter counter = Counter();
  mut Counter& writable = counter;
  increment(writable);
  Counter& read_only = counter;

  mut int value = 1;
  mut int& alias = value;
  alias += 2;
  if (read(read_only) == 1 and value == 3) {
    return 0;
  }
  return 1;
}
)";
}

struct NamedTest {
  std::string_view name;
  void (*run)();
};

template <std::size_t N>
int runNamedTests(int argc, char **argv, const NamedTest (&tests)[N],
                  std::string_view suite) {
  if (argc == 2 && std::string_view(argv[1]) == "--list") {
    for (const NamedTest &test : tests) {
      std::cout << test.name << '\n';
    }
    return 0;
  }
  if (argc == 3 && std::string_view(argv[1]) == "--test") {
    const std::string_view requested = argv[2];
    for (const NamedTest &test : tests) {
      if (test.name == requested) {
        test.run();
        return failures == 0 ? 0 : 1;
      }
    }
    std::cerr << "unknown " << suite << " test: " << requested << '\n';
    return 2;
  }
  if (argc != 1) {
    std::cerr << "usage: " << argv[0] << " [--list | --test NAME]\n";
    return 2;
  }
  for (const NamedTest &test : tests) {
    test.run();
  }
  if (failures != 0) {
    std::cerr << failures << ' ' << suite << " compiler test(s) failed\n";
    return 1;
  }
  std::cout << suite << " compiler tests passed (" << N << ")\n";
  return 0;
}

} // namespace
