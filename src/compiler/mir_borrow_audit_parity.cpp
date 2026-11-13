#include "mir_borrow_audit_parity.h"

#include "gti/ast.h"
#include "gti/source_graph.h"
#include "mir_borrow_audits.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <tuple>

namespace lang {
namespace {

thread_local MirBorrowAuditParityScope *activeScope = nullptr;

constexpr const char *rawKind = "raw";
constexpr const char *storageKind = "storage";

bool rawFinding(const Diagnostic &diagnostic) {
  return diagnostic.code == "GTI-S2102" &&
         diagnostic.message.find("live incompatible checked borrow") !=
             std::string::npos;
}

bool storageFinding(const Diagnostic &diagnostic) {
  return diagnostic.code == "GTI-S2045" &&
         (diagnostic.message.rfind("This call accesses persistent storage",
                                   0) == 0 ||
          diagnostic.message.rfind(
              "This call has unresolved GTI storage effects", 0) == 0);
}

const HirBody *hirBody(const HirProgram &hir, MirBodyAddress address) {
  const auto find = [&](const auto &instances) -> const HirBody * {
    for (const auto &instance : instances)
      if (instance.id == address.owner)
        return &instance.body;
    return nullptr;
  };
  switch (address.kind) {
  case MirBodyKind::Function:
    return find(hir.functionInstances());
  case MirBodyKind::Constructor:
    return find(hir.constructorInstances());
  case MirBodyKind::Destructor:
    return find(hir.destructorInstances());
  case MirBodyKind::Lambda:
    return find(hir.lambdaInstances());
  default:
    return nullptr;
  }
}

std::optional<SourceSpan> findingSpan(const HirProgram &hir,
                                      const MirBorrowAuditFinding &finding) {
  const HirBody *body = hirBody(hir, finding.body);
  if (body == nullptr)
    return std::nullopt;
  for (const HirValueId id : {finding.hirValue, finding.callSite}) {
    const HirValue *value = id == 0 ? nullptr : body->findValue(id);
    while (value != nullptr && value->source == nullptr &&
           value->operands.size() == 1)
      value = body->findValue(value->operands.front());
    if (value != nullptr && value->source != nullptr) {
      const Token token = expressionAnchorToken(*value->source);
      if (!token.lexeme.empty())
        return tokenSpan(token);
    }
  }
  for (const HirBinding &binding : body->bindings)
    if (finding.binding != 0 && binding.id == finding.binding &&
        binding.variable != nullptr)
      return tokenSpan(binding.variable->name());
  if (const HirStatement *statement =
          finding.hirStatement == 0 ? nullptr
                                    : body->findStatement(finding.hirStatement);
      statement != nullptr && statement->source != nullptr &&
      statement->source->extent())
    return *statement->source->extent();
  return std::nullopt;
}

} // namespace

MirBorrowAuditParityScope::MirBorrowAuditParityScope(
    const FrontendOptions &options) {
  const char *setting = std::getenv("GTI_PARITY_RAW_AUDIT");
  if (setting == nullptr || *setting == '\0')
    return;
  destination = setting;
  rawAudit = options.rawPointerAudit != RawPointerAuditMode::Off;
  reportLibrary = options.reportLibraryAuditFindings;
  previous = activeScope;
  activeScope = this;
}

MirBorrowAuditParityScope::~MirBorrowAuditParityScope() {
  if (activeScope == this)
    activeScope = previous;
}

MirBorrowAuditParityScope *MirBorrowAuditParityScope::active() {
  return activeScope;
}

bool MirBorrowAuditParityScope::semanticFindings(
    std::vector<Diagnostic> &diagnostics, std::size_t begin) {
  semantic.clear();
  original.clear();
  forced = false;
  bool otherError = false;
  bool storageError = false;
  for (std::size_t index = begin; index < diagnostics.size(); ++index) {
    const Diagnostic &diagnostic = diagnostics[index];
    const bool storage = storageFinding(diagnostic);
    if ((rawAudit && rawFinding(diagnostic)) || storage)
      semantic.push_back(diagnostic);
    if (diagnostic.severity == DiagnosticSeverity::Error) {
      storageError |= storage;
      otherError |= !storage;
    }
  }
  if (!storageError || otherError)
    return false;
  original = diagnostics;
  for (std::size_t index = diagnostics.size(); index-- > begin;)
    if (storageFinding(diagnostics[index])) {
      diagnostics.erase(diagnostics.begin() +
                        static_cast<std::ptrdiff_t>(index));
    }
  forced = true;
  return true;
}

void MirBorrowAuditParityScope::observe(const MirProgram &program,
                                        const HirProgram &hir) {
  if (observed)
    return;
  observed = true;
  const auto add = [&](const char *kind,
                       const std::vector<MirBorrowAuditFinding> &findings) {
    for (const auto &finding : findings) {
      const auto span = findingSpan(hir, finding);
      std::ostringstream detail;
      detail << "body=" << static_cast<int>(finding.body.kind) << ':'
             << finding.body.owner << " bb" << finding.block << ":i"
             << finding.instruction << " loans=";
      for (const auto loan : finding.loans)
        detail << loan << ',';
      detail << ' ' << finding.detail;
      mir.push_back(
          {kind, span.value_or(SourceSpan{"?", 0, 0, 0}), detail.str()});
    }
  };
  if (rawAudit)
    add(rawKind, auditMirRawAccesses(program));
  add(storageKind, auditMirPersistentStorageBorrows(program));
}

void MirBorrowAuditParityScope::finish(FrontendResult &result) {
  if (activeScope != this)
    return;
  const SourceUnitId entry = result.sourceGraph.entryUnit();
  const auto reported = [&](const SourceSpan &span) {
    if (reportLibrary)
      return true;
    const SourceUnitId unit = result.sourceGraph.sourceUnitForPath(span.source);
    return unit == entry || !result.sourceGraph.isCompilerTrusted(unit);
  };
  using Key = std::tuple<std::string, std::string, int>;
  std::map<Key, std::vector<std::string>> semanticSites;
  std::map<Key, std::vector<std::string>> mirSites;
  for (const auto &diagnostic : semantic) {
    const char *kind = rawFinding(diagnostic) ? rawKind : storageKind;
    if (kind == rawKind && !reported(diagnostic.primary))
      continue;
    semanticSites[{kind, diagnostic.primary.source, diagnostic.primary.line}]
        .push_back(diagnostic.message);
  }
  for (const auto &finding : mir) {
    if (finding.kind == rawKind && finding.span.source != "?" &&
        !reported(finding.span))
      continue;
    mirSites[{finding.kind, finding.span.source, finding.span.line}].push_back(
        finding.detail);
  }
  std::ostringstream out;
  std::string name = "?";
  for (const auto &unit : result.sourceGraph.sourceUnits())
    if (unit.id == entry)
      name = unit.path.string();
  std::map<std::string, std::array<int, 3>> counts;
  for (const auto &[key, messages] : semanticSites) {
    const bool match = mirSites.contains(key);
    ++counts[std::get<0>(key)][match ? 0 : 1];
    out << "PARITY " << std::get<0>(key) << (match ? " match " : " sem-only ")
        << std::get<1>(key) << ':' << std::get<2>(key) << '\n';
  }
  for (const auto &[key, details] : mirSites) {
    if (semanticSites.contains(key))
      continue;
    ++counts[std::get<0>(key)][2];
    for (const auto &detail : details)
      out << "PARITY " << std::get<0>(key) << " mir-only " << std::get<1>(key)
          << ':' << std::get<2>(key) << ' ' << detail << '\n';
  }
  for (const char *kind : {rawKind, storageKind}) {
    if (kind == rawKind && !rawAudit)
      continue;
    const auto &count = counts[kind];
    out << "PARITY-SUMMARY " << kind << ' ' << name << " match=" << count[0]
        << " sem-only=" << count[1] << " mir-only=" << count[2]
        << " mir-observed=" << (observed ? 1 : 0)
        << " forced=" << (forced ? 1 : 0)
        << " semantic-valid=" << (result.semanticValid ? 1 : 0)
        << " hir-valid=" << (result.hirValid ? 1 : 0) << '\n';
  }
  if (forced && !observed) {
    for (const auto &diagnostic : result.diagnostics)
      out << "PARITY-UNOBSERVED " << diagnostic.code << ' '
          << diagnostic.message.substr(0, 120) << '\n';
  }
  // Inline test sources reuse one name; keep the text of a mismatch.
  if (std::any_of(counts.begin(), counts.end(), [](const auto &entry) {
        return entry.second[1] != 0 || entry.second[2] != 0;
      }))
    if (const std::string *text = result.sources.find(name))
      out << "PARITY-SOURCE-BEGIN\n" << *text << "\nPARITY-SOURCE-END\n";
  if (destination == "1") {
    std::cerr << out.str();
  } else {
    std::ofstream file(destination, std::ios::app);
    file << out.str();
  }
  if (forced) {
    // Restore the result of the semantic rejection the measurement set
    // aside: its diagnostics, and no HIR or MIR.
    result.diagnostics = std::move(original);
    result.semanticValid = false;
    result.hirValid = false;
    result.mirValid = false;
    result.mirBorrowRejected = false;
    result.hir = {};
    result.mir = {};
  }
  activeScope = previous;
}

} // namespace lang
