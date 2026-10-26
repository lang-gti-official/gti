#include "gti/frontend.h"
#include "gti/lowered_program.h"
#include "gti/lowered_program_builder.h"
#include "gti/lowered_program_printer.h"
#include "gti/mir_lowering.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"
#include "gti/optimizer.h"
#include "gti/semantic_analyzer.h"
#include "gti/source_debug.h"

#include "../src/compiler/mir_source_debug.h"
#include "../src/compiler/mir_verification_queries.h"
#include "../src/compiler/mir_verification_support.h"
#include "lowered_program_contract_client.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

static_assert(std::is_same_v<decltype(lang::MirCAbiRecordFieldLayout::field),
                             lang::SymbolId>);
static_assert(
    std::is_same_v<decltype(lang::MirUnionFieldLayout::field), lang::SymbolId>);

namespace lang {

struct SemanticModelTestAccess {
  static auto &debug(SemanticModel &model) { return model.sourceDebug_; }
};

struct LoweredProgramTestAccess {
  static std::vector<LoweredBody> &bodies(LoweredProgram &program) {
    return program.bodies_;
  }

  static std::vector<LoweredDeclaration> &
  declarations(LoweredProgram &program) {
    return program.declarations_;
  }

  static std::vector<LoweredGeneratedItem> &
  generatedItems(LoweredProgram &program) {
    return program.generatedItems_;
  }

  static std::vector<LoweredSymbol> &symbols(LoweredProgram &program) {
    return program.symbols_;
  }

  static std::vector<LoweredFunctionInstance> &
  functionInstances(LoweredProgram &program) {
    return program.functionInstances_;
  }

  static std::vector<LoweredConstructorInstance> &
  constructorInstances(LoweredProgram &program) {
    return program.constructorInstances_;
  }

  static std::vector<LoweredLambdaInstance> &
  lambdaInstances(LoweredProgram &program) {
    return program.lambdaInstances_;
  }

  static MirProgram &mir(LoweredProgram &program) { return program.mir_; }
};

} // namespace lang

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct SourceDebugTestTables {
  std::vector<int> rows;
  friend bool operator==(const SourceDebugTestTables &,
                         const SourceDebugTestTables &) = default;
};

void testSourceDebugRecordOwnership() {
  using lang::SourceDebugCollection;
  using Record = lang::SourceDebugRecord<SourceDebugTestTables>;
  Record uncollected;
  expect(lang::isValidSourceDebugRecord(uncollected) && !uncollected.tables,
         "uncollected records have no table storage");
  Record notApplicable;
  notApplicable.collection = SourceDebugCollection::NotApplicable;
  expect(lang::isValidSourceDebugRecord(notApplicable) && !notApplicable.tables,
         "not-applicable records have no table storage");
  const Record uncollectedCopy(uncollected);
  const Record notApplicableCopy(notApplicable);
  expect(uncollectedCopy == uncollected && !uncollectedCopy.tables &&
             notApplicableCopy == notApplicable && !notApplicableCopy.tables,
         "copying absent collection preserves state without allocating tables");
  Record collected;
  collected.collection = SourceDebugCollection::CollectedLexicalV1;
  expect(!lang::isValidSourceDebugRecord(collected),
         "collected state requires tables, including for an empty body");
  collected.tables = std::make_unique<SourceDebugTestTables>();
  expect(lang::isValidSourceDebugRecord(collected) &&
             collected.tables->rows.empty() && collected != uncollected,
         "collected empty tables differ from uncollected facts");
  collected.tables->rows = {1, 2, 3};
  Record copy(collected);
  expect(copy == collected && copy.tables.get() != collected.tables.get() &&
             copy.tables->rows.data() != collected.tables->rows.data(),
         "copy construction deeply owns equal table contents");
  collected.tables->rows[0] = 9;
  expect(copy.tables->rows[0] == 1 && copy != collected,
         "mutating source rows does not alter a copied snapshot");
  Record assigned;
  assigned = copy;
  expect(assigned == copy && assigned.tables.get() != copy.tables.get(),
         "copy assignment does not share mutable tables");
  copy.tables->rows.push_back(4);
  expect(assigned.tables->rows.size() == 3,
         "copy-assigned table contents remain independent");
  const auto *owned = assigned.tables.get();
  Record moved(std::move(assigned));
  expect(moved.tables.get() == owned &&
             lang::isValidSourceDebugRecord(moved) &&
             lang::isValidSourceDebugRecord(assigned) && !assigned.tables,
         "move construction transfers tables and leaves a valid empty source");
  Record moveAssigned;
  moveAssigned = std::move(moved);
  expect(moveAssigned.tables.get() == owned &&
             lang::isValidSourceDebugRecord(moveAssigned) &&
             lang::isValidSourceDebugRecord(moved) && !moved.tables,
         "move assignment transfers tables and resets collection state");
  for (const auto state : {SourceDebugCollection::Uncollected,
                           SourceDebugCollection::NotApplicable,
                           static_cast<SourceDebugCollection>(255)}) {
    Record forged;
    forged.collection = state;
    forged.tables = std::make_unique<SourceDebugTestTables>();
    expect(!lang::isValidSourceDebugRecord(forged),
           "unexpected table storage and unknown collection states reject");
  }
  Record unknown;
  unknown.collection = static_cast<SourceDebugCollection>(255);
  expect(!lang::isValidSourceDebugRecord(unknown),
         "an unknown collection state also rejects without tables");
  expect(lang::isValidSourceDebugProfile(lang::SourceDebugProfile::None) &&
             lang::isValidSourceDebugProfile(
                 lang::SourceDebugProfile::LexicalV1) &&
             !lang::isValidSourceDebugProfile(
                 static_cast<lang::SourceDebugProfile>(255)),
         "only the two reviewed collection profiles are valid");
}

void testSemanticSourceDebugProducers() {
  const auto syntax =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Parsing})
          .analyze("source-debug-semantic.gti", R"gti(
T identity<T>(T value) { T copy = value; return copy; }
void empty() {}
int32_t parent(int32_t input) {
  mut int32_t local = input;
  { int32_t local = 3; }
  if (input > 0) { local = input + 1; } else { local = input - 1; }
  mut int32_t index = 0;
  while (index < 2) { index++; }
  auto make = []() -> int32_t { int32_t inside = 7; return inside; };
  return local + make();
}
int main() { return parent(4); }
)gti");
  expect(syntax.syntaxValid, "source-debug producer fixture parses");
  if (!syntax.syntaxValid)
    return;
  lang::SemanticVisitor plain;
  lang::SemanticVisitor collected(
      lang::TargetInfo::host(), nullptr, true, nullptr, nullptr,
      lang::RawPointerAuditMode::Off, false,
      lang::TrustedBodyAnalysis::Complete, lang::SourceDebugProfile::LexicalV1);
  expect(plain.check(syntax.program), "disabled source-debug fixture analyzes");
  expect(collected.check(syntax.program),
         "collected source-debug fixture analyzes");
  const auto issues = collected.model().verifySourceDebug();
  for (const auto &issue : issues)
    std::cerr << "source debug: " << issue << '\n';
  expect(issues.empty(), "accepted source producers pass exact-domain checks");
  expect(plain.model().sourceDebugBodies().empty() &&
             !lang::SemanticModelTestAccess::debug(
                  const_cast<lang::SemanticModel &>(plain.model()))
                  .tables,
         "disabled semantic collection owns no source tables");
  const auto &plainSymbols = plain.model().database().symbols();
  const auto &debugSymbols = collected.model().database().symbols();
  bool sameSymbols = plainSymbols.size() == debugSymbols.size();
  for (std::size_t i = 0; sameSymbols && i < plainSymbols.size(); ++i)
    sameSymbols = plainSymbols[i].id == debugSymbols[i].id &&
                  plainSymbols[i].name == debugSymbols[i].name &&
                  plainSymbols[i].type == debugSymbols[i].type;
  expect(
      sameSymbols && plain.errors().size() == collected.errors().size(),
      "collection preserves semantic symbol identities/types and diagnostics");
  const auto &bodies = collected.model().sourceDebugBodies();
  const lang::SemanticSourceDebugBody *parent = nullptr;
  const lang::SemanticSourceDebugBody *empty = nullptr;
  const lang::SemanticSourceDebugBody *lambda = nullptr;
  lang::FunctionId generic = 0;
  for (const auto &body : bodies) {
    if (body.kind == lang::SourceDebugBodyKind::Lambda) {
      lambda = &body;
      continue;
    }
    if (body.kind != lang::SourceDebugBodyKind::Function)
      continue;
    const auto *source = static_cast<const lang::FunctionDecl *>(body.source);
    if (source->name().lexeme == "parent")
      parent = &body;
    if (source->name().lexeme == "empty")
      empty = &body;
    if (source->name().lexeme == "identity")
      generic = collected.model().findFunction(*source)->id;
  }
  expect(empty && empty->facts.tables &&
             empty->facts.collection ==
                 lang::SourceDebugCollection::CollectedLexicalV1 &&
             empty->facts.tables->locals.empty() &&
             empty->facts.tables->statements.empty(),
         "an analyzed empty body is collected with real empty facts");
  expect(parent && lambda && lambda->creator == parent->source &&
             lambda->facts.tables->scopes.front().parent == 0,
         "lambda has an exact creator and an independent lexical root");
  if (parent) {
    std::vector<lang::SourceScopeId> shadowed;
    for (const auto &local : parent->facts.tables->locals)
      if (local.name == "local")
        shadowed.push_back(local.scope);
    expect(shadowed.size() == 2 && shadowed[0] != shadowed[1],
           "shadowed declarations retain distinct accepted lexical scopes");
    auto copy = collected.model();
    auto &rows = lang::SemanticModelTestAccess::debug(copy).tables->bodies;
    auto found = std::find_if(rows.begin(), rows.end(), [&](const auto &body) {
      return body.source == parent->source;
    });
    expect(found != rows.end() &&
               found->facts.tables.get() != parent->facts.tables.get(),
           "semantic model copies deeply own phase-body tables");
    if (found != rows.end()) {
      const auto saved = *found;
      found->facts.collection = lang::SourceDebugCollection::Uncollected;
      expect(!copy.verifySourceDebug().empty(),
             "forged collected-state pair rejects");
      *found = saved;
      found->facts.tables->scopes.front().parent = 1;
      expect(!copy.verifySourceDebug().empty(), "cyclic source parent rejects");
      *found = saved;
      found->facts.tables->snapshot++;
      expect(!copy.verifySourceDebug().empty(),
             "foreign source snapshot rejects");
      *found = saved;
      found->facts.tables->source = empty->source;
      expect(!copy.verifySourceDebug().empty(), "foreign source body rejects");
      *found = saved;
      found->facts.tables->locals.front().type = lang::SemanticType::Bool;
      expect(!copy.verifySourceDebug().empty(),
             "forged concrete binding type rejects");
      *found = saved;
      expect(
          copy.verifySourceDebug().empty() &&
              parent->facts.tables->locals.front().type !=
                  lang::SemanticType::Bool,
          "forgery controls leave the independent source snapshot unchanged");
    }
  }
  auto stripped = collected.model();
  lang::SemanticModelTestAccess::debug(stripped) = {};
  expect(!stripped.verifySourceDebug().empty(),
         "a stripped semantic snapshot cannot satisfy LexicalV1 capability");
  auto lowered = lang::HirLowerer(lang::TargetInfo::host())
                     .lower(syntax.program, collected.model(), collected);
  expect(lowered.program.valid() &&
             lang::verifyHirSourceDebug(collected.model(), lowered.program)
                 .valid(),
         "concrete HIR retains its exact accepted source scopes and bindings");
  if (lowered.program.valid()) {
    for (const auto &function : lowered.program.functionInstances()) {
      if (!function.body.sourceDebug.tables ||
          function.body.sourceDebug.tables->locals.empty())
        continue;
      auto &body = const_cast<lang::HirBody &>(function.body);
      const auto saved = body.sourceDebug;
      body.sourceDebug.tables->locals.front().type = lang::SemanticType::Bool;
      expect(!lang::verifyHirSourceDebug(collected.model(), lowered.program)
                  .valid(),
             "HIR cannot forge its concrete source-local type");
      body.sourceDebug = saved;
      body.sourceDebug.tables->domain.body++;
      expect(!lang::verifyHirSourceDebug(collected.model(), lowered.program)
                  .valid(),
             "HIR cannot transplant a source table into a foreign place body");
      body.sourceDebug = saved;
      body.sourceDebug.collection = lang::SourceDebugCollection::NotApplicable;
      body.sourceDebug.tables.reset();
      expect(!lang::verifyHirSourceDebug(collected.model(), lowered.program)
                  .valid(),
             "a real source HIR body cannot claim not-applicable collection");
      body.sourceDebug = saved;
      break;
    }
    expect(
        lang::verifyHirSourceDebug(collected.model(), lowered.program).valid(),
        "HIR forgery controls restore the deep-owned source snapshot");
  }
  expect(generic != 0, "generic source body was collected");
  if (generic != 0) {
    const auto narrow = collected.analyzeFunctionInstance(
        generic, {}, {}, {lang::SemanticType::Int32}, {});
    const auto wide = collected.analyzeFunctionInstance(
        generic, {}, {}, {lang::SemanticType::Int64}, {});
    expect(narrow.model.verifySourceDebug().empty() &&
               wide.model.verifySourceDebug().empty(),
           "concrete analyses verify in their own accepted domains");
    const auto &left = narrow.model.sourceDebugBodies();
    const auto &right = wide.model.sourceDebugBodies();
    expect(!left.empty() && !right.empty() &&
               left.front().facts.tables->instance !=
                   right.front().facts.tables->instance &&
               left.front().facts.tables->locals.front().type ==
                   lang::SemanticType::Int32 &&
               right.front().facts.tables->locals.front().type ==
                   lang::SemanticType::Int64,
           "same source spelling retains exact concrete substitution and local "
           "type");
    if (!left.empty() && !right.empty()) {
      auto forged = narrow.model;
      lang::SemanticModelTestAccess::debug(forged)
          .tables->bodies.front()
          .facts = right.front().facts;
      expect(!forged.verifySourceDebug().empty(),
             "a sibling concrete instance cannot supply this body's facts");
    }
  }
}

void testFrontendSourceDebugProfile() {
  const auto root =
      std::filesystem::temp_directory_path() /
      ("gti-o15-profile-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto library = root / "stdlib";
  std::filesystem::create_directories(library);
  struct FixtureCleanup {
    std::filesystem::path root;
    ~FixtureCleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
    }
  } cleanup{root};
  const auto prelude = library / "prelude.gti";
  const auto entry = root / "main.gti";
  const std::unordered_map<std::string, std::string> overrides{
      {prelude.string(),
       "T cached<T>(T input) { T local = input; return local; }\n"}};
  std::ofstream(prelude) << overrides.at(prelude.string());
  lang::FrontendOptions options;
  options.cache = std::make_shared<lang::FrontendCache>();
  options.preambleCache = std::make_shared<lang::SemanticPreambleCache>();
  options.instanceCache = std::make_shared<lang::InstanceAnalysisCache>();
  options.trustedBodies = lang::TrustedBodyAnalysis::Reachable;
  const auto run = [&](lang::SourceDebugProfile profile, bool expectWarm) {
    auto selected = options;
    lang::FrontendWorkMetrics metrics;
    selected.sourceDebugProfile = profile;
    selected.workMetrics = &metrics;
    auto result = lang::Frontend(selected).analyze(
        entry, "int main() { int value = cached<int>(7); return value; }\n",
        {prelude}, overrides, {library});
    if (!result.canGenerateCode())
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << "frontend source profile: " << diagnostic.message << '\n';
    expect(result.canGenerateCode() &&
               result.semantics.sourceDebugProfile() == profile &&
               result.hir.sourceDebugProfile() == profile &&
               result.mir.sourceDebugProfile() == profile,
           "frontend binds the requested profile before semantic and HIR/MIR "
           "production");
    expect(metrics.semanticPreambleAdoptions != 0 &&
               (expectWarm ? metrics.semanticPreambleBuilds == 0
                           : metrics.semanticPreambleBuilds != 0),
           "preamble reuse is isolated by the exact source-debug capability");
    if (!result.canGenerateCode())
      return;
    expect(lang::verifyHirSourceDebug(result.semantics, result.hir).valid(),
           "cached source profile retains verified concrete source facts");
    for (const auto address : lang::enumerateMirBodyAddresses(result.mir)) {
      const auto *body = lang::findMirBody(result.mir, address);
      if (profile == lang::SourceDebugProfile::None)
        expect(!body->sourceDebug.tables,
               "None cache reuse allocates no MIR source tables");
    }
    if (profile == lang::SourceDebugProfile::LexicalV1)
      expect(result.mir.module().sourceDebug.tables &&
                 result.mir.module().sourceDebug.tables->locals.empty(),
             "the independently empty module inventory remains explicitly "
             "collected");
  };
  run(lang::SourceDebugProfile::None, false);
  run(lang::SourceDebugProfile::LexicalV1, false);
  run(lang::SourceDebugProfile::LexicalV1, true);
  run(lang::SourceDebugProfile::None, true);

  const auto syntax =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Parsing})
          .analyze("source-debug-preamble.gti",
                   "int helper(int value) { return value; }");
  lang::SemanticVisitor plain;
  const auto uncollected =
      plain.buildPreamble(lang::Program(syntax.program.declarations()));
  expect(lang::semanticPreambleSupportsSourceDebug(
             *uncollected, lang::SourceDebugProfile::None) &&
             !lang::semanticPreambleSupportsSourceDebug(
                 *uncollected, lang::SourceDebugProfile::LexicalV1),
         "a matching cache key cannot grant missing preamble debug capability");
  lang::SemanticVisitor collected(
      lang::TargetInfo::host(), nullptr, true, nullptr, nullptr,
      lang::RawPointerAuditMode::Off, false,
      lang::TrustedBodyAnalysis::Complete, lang::SourceDebugProfile::LexicalV1);
  collected.adoptPreamble(uncollected, syntax.program.declarations().size());
  expect(collected.check(syntax.program) &&
             collected.model().sourceDebugProfile() ==
                 lang::SourceDebugProfile::LexicalV1 &&
             !collected.model().sourceDebugBodies().empty() &&
             collected.model().verifySourceDebug().empty(),
         "an incompatible direct preamble adoption falls back to complete "
         "analysis");
}

void testMirSourceDebugTransport() {
  const auto syntax =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Parsing})
          .analyze("source-debug-mir.gti", R"gti(
int probe(int input) {
  mut int result = input;
  { mut int local = input; local = result; result = local; 7; }
  return result;
}
int main() { return probe(4); }
)gti");
  expect(syntax.syntaxValid, "MIR source-debug fixture parses");
  if (!syntax.syntaxValid)
    return;
  lang::SemanticVisitor semantics(
      lang::TargetInfo::host(), &syntax.sourceGraph, true, nullptr,
      &syntax.sources, lang::RawPointerAuditMode::Off, false,
      lang::TrustedBodyAnalysis::Complete, lang::SourceDebugProfile::LexicalV1);
  const bool analyzed = semantics.check(syntax.program) &&
                        semantics.applyProgramRequest(syntax.program, {});
  for (const auto &diagnostic : semantics.errors())
    std::cerr << "MIR source semantic: " << diagnostic.message << '\n';
  expect(analyzed, "MIR source-debug fixture analyzes");
  if (!analyzed)
    return;
  auto hir = lang::HirLowerer(lang::TargetInfo::host())
                 .lower(syntax.program, semantics.model(), semantics);
  expect(hir.program.valid(), "MIR source-debug fixture has verified HIR");
  if (!hir.program.valid())
    return;
  auto metadata = lang::FailureMetadataBuilder().build(
      syntax.sourceGraph, syntax.sources, hir.program, "source-debug-mir.gti");
  expect(metadata.valid(),
         "MIR source-debug fixture retains real failure metadata");
  if (!metadata.valid())
    return;
  auto lowered = lang::MirLowerer().lower(hir.program, metadata.metadata);
  for (const auto &error : lowered.errors)
    std::cerr << "MIR source debug: " << error.message << '\n';
  expect(lowered.valid() && lowered.program.sourceDebugProfile() ==
                                lang::SourceDebugProfile::LexicalV1,
         "MIR retains the requested source profile");
  if (!lowered.valid())
    return;
  bool exercised = false;
  for (const auto &function : hir.program.functionInstances()) {
    if (!function.source || function.source->name().lexeme != "probe")
      continue;
    const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                       function.id};
    const auto *body = lang::findMirBody(lowered.program, address);
    expect(body && body->sourceDebug.tables &&
               lang::mir_detail::sourceDebugMatchesHir(function.body, *body),
           "MIR owns exact flattened source declarations and memberships");
    if (!body || !body->sourceDebug.tables)
      continue;
    const auto &tables = *body->sourceDebug.tables;
    const auto local = std::find_if(
        tables.locals.begin(), tables.locals.end(),
        [](const auto &row) { return row.declaration.name == "local"; });
    expect(local != tables.locals.end() && local->place != 0,
           "ordinary trivial local retains its existing binding place");
    if (local == tables.locals.end() || !local->place)
      continue;
    std::vector<lang::MirDebugAvailabilityRequest> requests;
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Initialize &&
            instruction.destination == local->place)
          requests.push_back(
              {local->declaration.id, {block.id, instruction.id, true}});
    const auto observed =
        lang::queryMirDebugAvailability(lowered.program, address, requests);
    expect(!requests.empty() && observed.verification.valid() &&
               observed.requested.front().readability ==
                   lang::MirDebugReadability::DefinitelyReadable,
           "source-local facade joins exact identity before canonical "
           "availability");
    expect(std::any_of(tables.localEnds.begin(), tables.localEnds.end(),
                       [&](const auto &end) {
                         return end.local == local->declaration.id &&
                                std::find(observed.requiredStops.begin(),
                                          observed.requiredStops.end(),
                                          end) != observed.requiredStops.end();
                       }),
           "a trivial lexical scope end is a required cut without a Drop "
           "instruction");
    for (const auto &end : tables.localEnds)
      if (end.local == local->declaration.id) {
        const std::vector<lang::MirDebugAvailabilityRequest> atEnd{
            {end.local, end.point}};
        const auto ended =
            lang::queryMirDebugAvailability(lowered.program, address, atEnd);
        expect(
            ended.verification.valid() && ended.requested.size() == 1 &&
                ended.requested.front().readability ==
                    lang::MirDebugReadability::Unavailable &&
                ended.requested.front().reason ==
                    lang::MirDebugAvailabilityReason::OutOfScope,
            "source scope clips a still-initialized trivial execution place");
      }
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions)
        expect(instruction.kind != lang::MirInstructionKind::Drop,
               "descriptive local exits do not synthesize runtime destruction");
    auto copy = *body;
    expect(copy == *body &&
               copy.sourceDebug.tables.get() != body->sourceDebug.tables.get(),
           "MIR source tables deep-copy equal content");
    copy.sourceDebug.tables->locals[local->declaration.id - 1]
        .declaration.type = lang::SemanticType::Bool;
    expect(
        !lang::verifyMirBody(copy, function.id).valid(),
        "MIR rejects a source-local type forged against its execution place");
    copy = *body;
    copy.sourceDebug.tables->locals.front().declaration.name += "forged";
    expect(!lang::mir_detail::sourceDebugMatchesHir(function.body, copy),
           "source MIR cannot forge a declaration copied from HIR");
    expect(lang::MirPrinter().print(copy) != lang::MirPrinter().print(*body),
           "MIR serialization includes every source declaration field");
    copy = *body;
    copy.sourceDebug.tables->localEnds.front().point.block = 0;
    expect(!lang::verifyMirBody(copy, function.id).valid(),
           "MIR rejects foreign lexical-end points");
    const std::vector<lang::MirDebugAvailabilityRequest> invalid{
        {0, {body->entry, 0, false, true}}};
    expect(!lang::queryMirDebugAvailability(lowered.program, address, invalid)
                .verification.valid(),
           "source-local facade rejects a forged identity");
    auto promoted = *body;
    const auto promotion = lang::rewriteMirScalarPlace(promoted, local->place);
    expect(
        promotion.has_value() &&
            lang::verifyMirBody(promoted, function.id).valid() &&
            promoted.sourceDebug.tables->locals[local->declaration.id - 1]
                    .policy == lang::MirSourceLocalPolicy::OptimizedOut &&
            promoted.sourceDebug.tables->locals[local->declaration.id - 1]
                    .place == 0,
        "promotion remains eligible and makes the removed storage unavailable");
    bool deadBoundary = false;
    // A structural mutator fixture, not frontend/native admission evidence:
    // place a neutral unused literal at a synthetic lexical cut. Real source
    // statement tails also own full-expression ends and must not be deleted.
    auto deadFixture = lowered.program;
    auto *fixtureBody = lang::findMirBody(deadFixture, address);
    lang::MirInstruction neutral;
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions) {
        neutral.id = std::max(neutral.id, instruction.id + 1);
        if (instruction.operation == lang::MirOperation::Literal &&
            instruction.result) {
          neutral.kind = lang::MirInstructionKind::Compute;
          neutral.operation = lang::MirOperation::Literal;
          neutral.hirValue = instruction.hirValue;
          neutral.literal = instruction.literal;
          neutral.literalProvenance = instruction.literalProvenance;
          neutral.info = instruction.info;
        }
      }
    if (neutral.literal) {
      auto &block = fixtureBody->blocks.back();
      neutral.result = fixtureBody->values.size() + 1;
      fixtureBody->values.push_back({.id = *neutral.result,
                                     .sourceValue = neutral.hirValue,
                                     .info = neutral.info,
                                     .definitionBlock = block.id,
                                     .definition = neutral.id});
      block.instructions.push_back(neutral);
      fixtureBody->sourceDebug.tables->localEnds.push_back(
          {local->declaration.id, {block.id, neutral.id, true}});
      expect(lang::rebuildMirValueUses(*fixtureBody),
             "synthetic fixture rebuilds value uses");
      const auto fixtureProof = lang::verifyMirBody(*fixtureBody, function.id);
      for (const auto &error : fixtureProof.errors)
        std::cerr << "synthetic debug cut: " << error.message << '\n';
      expect(fixtureProof.valid(), "synthetic dead-mutator fixture has "
                                   "structurally valid execution MIR");
      auto candidate = deadFixture;
      auto *candidateBody = lang::findMirBody(candidate, address);
      const auto before = *candidateBody;
      const auto repaired = lang::mir_detail::rewriteVerifiedMirDeadInstruction(
          candidate, address, neutral.id);
      expect(repaired.has_value() &&
                 lang::verifyMirBody(*candidateBody, function.id).valid() &&
                 candidateBody->sourceDebug.tables
                         ->locals[local->declaration.id - 1]
                         .policy == lang::MirSourceLocalPolicy::OptimizedOut,
             "the actual dead mutator repairs a removed lexical anchor");
      candidate = deadFixture;
      candidateBody = lang::findMirBody(candidate, address);
      candidateBody->sourceDebug.tables->domain.body++;
      const auto refused = *candidateBody;
      expect(!lang::mir_detail::rewriteVerifiedMirDeadInstruction(
                 candidate, address, neutral.id) &&
                 *candidateBody == refused &&
                 lang::MirPrinter().print(*candidateBody) ==
                     lang::MirPrinter().print(refused),
             "refused in-place mutation restores code and source rows "
             "byte-identically");
      candidateBody->sourceDebug.tables->domain =
          before.sourceDebug.tables->domain;
      expect(*candidateBody == before,
             "repair refusal leaves no hidden debug policy edits");
      deadBoundary = true;
    }
    expect(deadBoundary,
           "dead-mutation test constructs its structural cut fixture");
    exercised = true;
  }
  expect(exercised, "MIR source-debug transport controls execute");
}

void testDebugPlaceAvailability() {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto frontend =
      lang::Frontend(
          {.sourceDebugProfile = lang::SourceDebugProfile::LexicalV1})
          .analyze("debug-place-availability.gti", R"gti(
#include <std/memory>
int probe(mut int input, bool choose) {
  mut int local = input;
  int moved = std::move(local);
  local = input;
  if (choose) { local = input + 1; }
  double untracked = double(input);
  return local + moved;
}
void joined(bool choose, mut int local) {
  if (choose) { [[discard]] std::move(local); }
  local = 7;
}
int repeated(mut int count) {
  mut int result = 0;
  while (count > 0) {
    int iteration = count;
    result += std::move(iteration);
    count--;
  }
  return result;
}
int main() { return probe(4, true); }
)gti",
                   {library / "prelude.gti"}, {}, {library});
  expect(frontend.canGenerateCode(), "debug place query fixture lowers");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
    return;
  }
  const lang::MirFunctionInstance *function = nullptr;
  for (const auto &candidate : frontend.hir.functionInstances())
    if (candidate.source && candidate.source->name().lexeme == "probe")
      function = frontend.mir.findFunctionInstance(candidate.id);
  expect(function != nullptr, "query fixture retains its source function");
  if (!function)
    return;
  const auto &body = function->body;
  lang::MirPlaceId local = 0;
  lang::MirPlaceId untracked = 0;
  for (const auto &place : body.places) {
    if (place.root != lang::MirPlaceRootKind::Binding ||
        !place.projections.empty())
      continue;
    const auto *symbol = frontend.semantics.database().findSymbol(place.symbol);
    if (symbol && symbol->name == "local")
      local = place.id;
    if (symbol && symbol->name == "untracked")
      untracked = place.id;
  }
  expect(local != 0 && untracked != 0, "queries select exact binding roots");
  if (!local || !untracked)
    return;
  using Query = lang::mir_detail::MirExistingPlaceQuery;
  using State = lang::mir_detail::MirDebugReadability;
  std::vector<Query> queries;
  std::vector<State> expected;
  std::optional<Query> movedStop;
  const auto sameLocal = [&](lang::MirPlaceId id) {
    const auto *place = body.findPlace(id);
    return place && place->key == body.findPlace(local)->key;
  };
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions) {
      const auto add = [&](bool after, State state) {
        queries.push_back({local, {block.id, instruction.id, after}});
        expected.push_back(state);
      };
      if (instruction.kind == lang::MirInstructionKind::Initialize &&
          instruction.destination && sameLocal(*instruction.destination) &&
          !instruction.operands.empty()) {
        add(false, State::Unavailable);
        add(true, State::DefinitelyReadable);
      }
      if (instruction.kind == lang::MirInstructionKind::Move &&
          !instruction.operands.empty() &&
          sameLocal(instruction.operands.front().place)) {
        add(false, State::DefinitelyReadable);
        add(true, State::Unavailable);
        movedStop = queries.back();
      }
      if (instruction.kind == lang::MirInstructionKind::Assign &&
          instruction.destination && sameLocal(*instruction.destination))
        add(true, State::DefinitelyReadable);
    }
  expect(queries.size() >= 5 && movedStop.has_value(),
         "queries cover initialize, move and reinitialize boundaries");
  queries.push_back({untracked, {body.entry, 0, false}});
  expected.push_back(State::Unknown);
  const auto result = lang::mir_detail::verifyMirBodyWithPlaceQueries(
      body, function->id, queries);
  expect(result.verification.valid() &&
             result.requested.size() == queries.size(),
         "fresh query uses the ordinary successful body verifier");
  for (std::size_t i = 0; i < result.requested.size(); ++i)
    expect(result.requested[i].readability == expected[i],
           "stable canonical transfer determines debug readability");
  expect(movedStop &&
             std::find(result.requiredStops.begin(), result.requiredStops.end(),
                       *movedStop) != result.requiredStops.end(),
         "a moved interior cuts ranges even when reinitialization restores it");
  auto lowered = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, lang::TargetInfo::host());
  expect(lowered.valid(),
         "move/reinitialize fixture constructs fresh source ranges");
  if (lowered.valid() && movedStop) {
    const auto *debug =
        lowered.program->findBody({lang::MirBodyKind::Function, function->id});
    const auto *block = body.findBlock(movedStop->point.block);
    const auto position = [&](lang::MirDebugPoint point) {
      if (point.blockEntry)
        return std::size_t{0};
      for (std::size_t i = 0; i < block->instructions.size(); ++i)
        if (block->instructions[i].id == point.instruction)
          return 2 * i + 1 + std::size_t(point.after);
      return 2 * block->instructions.size() + 1;
    };
    bool before = false;
    bool after = false;
    for (const auto &range : debug->sourceDebug.tables->ranges) {
      if (range.place != local || range.begin.block != block->id)
        continue;
      before |= position(range.end) <= position(movedStop->point);
      after |= position(range.begin) > position(movedStop->point);
      expect(!(position(range.begin) < position(movedStop->point) &&
               position(range.end) > position(movedStop->point)),
             "lowered ranges cannot bridge a moved interior with readable "
             "endpoints");
    }
    expect(before && after, "readable storage has distinct ranges before move "
                            "and after reinitialization");
  }
  auto forged = queries;
  forged.front().place = 0;
  const auto invalid = lang::mir_detail::verifyMirBodyWithPlaceQueries(
      body, function->id, forged);
  expect(!invalid.verification.valid() &&
             std::all_of(invalid.requested.begin(), invalid.requested.end(),
                         [](const auto &fact) {
                           return fact.readability == State::Unknown;
                         }),
         "foreign query identities never produce positive facts");
  expect(lang::mir_detail::verifyMirBodyWithPlaceQueries(body, function->id, {})
             .verification.valid(),
         "empty queries use ordinary verification without observations");

  forged = queries;
  forged.front().point.instruction = 0;
  forged.front().point.after = true;
  expect(!lang::mir_detail::verifyMirBodyWithPlaceQueries(body, function->id,
                                                          forged)
              .verification.valid(),
         "a terminator has no invented after-instruction point");
  auto malformed = body;
  malformed.entry = 0;
  const auto badBody = lang::mir_detail::verifyMirBodyWithPlaceQueries(
      malformed, function->id, queries);
  expect(!badBody.verification.valid() &&
             std::all_of(badBody.requested.begin(), badBody.requested.end(),
                         [](const auto &fact) {
                           return fact.readability == State::Unknown;
                         }) &&
             badBody.requiredStops.empty(),
         "failed body verification cannot leak positive observations");

  for (const auto &hir : frontend.hir.functionInstances()) {
    if (!hir.source || (hir.source->name().lexeme != "joined" &&
                        hir.source->name().lexeme != "repeated"))
      continue;
    const bool join = hir.source->name().lexeme == "joined";
    const auto *target = frontend.mir.findFunctionInstance(hir.id);
    const auto &flow = target->body;
    const auto wanted = join ? "local" : "iteration";
    bool observed = false;
    for (const auto &block : flow.blocks)
      for (const auto &instruction : block.instructions) {
        if (!instruction.destination ||
            instruction.kind != (join ? lang::MirInstructionKind::Assign
                                      : lang::MirInstructionKind::Initialize) ||
            instruction.operands.empty())
          continue;
        const auto *place = flow.findPlace(*instruction.destination);
        const auto *symbol =
            place ? frontend.semantics.database().findSymbol(place->symbol)
                  : nullptr;
        if (!symbol || symbol->name != wanted)
          continue;
        const std::vector<Query> points{
            {place->id, {block.id, instruction.id, false}},
            {place->id, {block.id, instruction.id, true}}};
        const auto facts = lang::mir_detail::verifyMirBodyWithPlaceQueries(
            flow, target->id, points);
        expect(facts.verification.valid() && facts.requested.size() == 2 &&
                   facts.requested[0].readability == State::Unavailable &&
                   facts.requested[1].readability == State::DefinitelyReadable,
               "conditional joins and loop backedges use converged flow facts");
        observed = true;
      }
    expect(observed, "join/loop query exercised the selected binding");
  }
}

void testDebugLifetimeAndBorrowAvailability() {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto frontend =
      lang::Frontend(
          {.sourceDebugProfile = lang::SourceDebugProfile::LexicalV1})
          .analyze("debug-lifetime-availability.gti", R"gti(
#include <std/memory>
class Owned {
public:
  mut int value = 3;
  ~Owned() { this.value = 0; }
};
void lifetime() {
  Owned first{};
  Owned second = std::move(first);
}
int borrowing() {
  mut int local = 4;
  { mut int& alias = local; alias += 1; }
  return local;
}
int main() { lifetime(); return borrowing() - 5; }
)gti",
                   {library / "prelude.gti"}, {}, {library});
  expect(frontend.canGenerateCode(),
         "lifetime/borrow observation fixture lowers");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
    return;
  }
  using Query = lang::mir_detail::MirExistingPlaceQuery;
  using State = lang::mir_detail::MirDebugReadability;
  bool sawLifetime = false;
  bool sawBorrow = false;
  for (const auto &hir : frontend.hir.functionInstances()) {
    if (!hir.source || (hir.source->name().lexeme != "lifetime" &&
                        hir.source->name().lexeme != "borrowing"))
      continue;
    const bool lifetime = hir.source->name().lexeme == "lifetime";
    const auto *function = frontend.mir.findFunctionInstance(hir.id);
    const auto &body = function->body;
    lang::MirPlaceId selected = 0;
    for (const auto &place : body.places) {
      const auto *symbol =
          frontend.semantics.database().findSymbol(place.symbol);
      if (place.root == lang::MirPlaceRootKind::Binding &&
          place.projections.empty() && symbol &&
          symbol->name == (lifetime ? "first" : "local"))
        selected = place.id;
    }
    expect(selected != 0, "availability selects the exact source binding");
    if (!selected)
      continue;
    std::vector<Query> requests;
    std::vector<State> expected;
    std::vector<Query> cuts;
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto append = [&](bool after, State state) {
          requests.push_back({selected, {block.id, instruction.id, after}});
          expected.push_back(state);
        };
        if (lifetime) {
          for (const auto &event : instruction.lifecycle) {
            const auto *source = body.findDropObligation(event.source);
            const auto *target = body.findDropObligation(event.target);
            if ((event.kind == lang::MirLifecycleEventKind::Initialize ||
                 event.kind == lang::MirLifecycleEventKind::Reparent) &&
                target && target->place == selected) {
              append(false, State::Unavailable);
              append(true, State::DefinitelyReadable);
              sawLifetime = true;
            }
            if (event.kind == lang::MirLifecycleEventKind::Move && source &&
                source->place == selected) {
              append(false, State::DefinitelyReadable);
              append(true, State::Unavailable);
              cuts.push_back(requests.back());
            }
          }
        } else if (instruction.loan) {
          const auto *loan = body.findLoan(*instruction.loan);
          const auto *origin = loan ? body.findPlace(loan->source) : nullptr;
          if (!loan || loan->access != lang::AccessMode::Mutable || !origin ||
              origin->key != body.findPlace(selected)->key)
            continue;
          if (instruction.kind == lang::MirInstructionKind::Borrow) {
            append(false, State::DefinitelyReadable);
            append(true, State::Unavailable);
            cuts.push_back(requests.back());
            sawBorrow = true;
          } else if (instruction.kind == lang::MirInstructionKind::EndBorrow) {
            append(false, State::Unavailable);
            append(true, State::DefinitelyReadable);
          }
        }
      }
    expect(requests.size() >= 4 && !cuts.empty(),
           "queries cover both availability transitions and interior cuts");
    const auto result = lang::mir_detail::verifyMirBodyWithPlaceQueries(
        body, function->id, requests);
    expect(result.verification.valid() &&
               result.requested.size() == expected.size(),
           "observations preserve the ordinary verifier result");
    for (std::size_t i = 0; i < result.requested.size(); ++i)
      expect(result.requested[i].readability == expected[i],
             "canonical lifecycle and loan states determine availability");
    for (const auto &cut : cuts)
      expect(std::find(result.requiredStops.begin(), result.requiredStops.end(),
                       cut) != result.requiredStops.end(),
             "inactive lifetime or exclusive borrowing cuts a source range");
    auto forged = body;
    if (lifetime && !forged.dropObligations.empty())
      forged.dropObligations.front().place = 0;
    else if (!forged.loans.empty())
      forged.loans.front().source = 0;
    const auto rejected = lang::mir_detail::verifyMirBodyWithPlaceQueries(
        forged, function->id, requests);
    expect(!rejected.verification.valid() && rejected.requiredStops.empty() &&
               std::all_of(rejected.requested.begin(), rejected.requested.end(),
                           [](const auto &fact) {
                             return fact.readability == State::Unknown;
                           }),
           "forged lifecycle/borrow identities cannot mint availability");
  }
  expect(sawLifetime && sawBorrow, "both canonical authorities are exercised");
}

void testDebugConcreteEntryAvailability() {
  const auto frontend =
      lang::Frontend(
          {.sourceDebugProfile = lang::SourceDebugProfile::LexicalV1})
          .analyze("debug-concrete-entry.gti", R"gti(
class Receiver {
public:
  int value = 4;
  int read() { return this.value; }
};
int packed<Args...>(int first, Args... rest) { return first; }
int main() { Receiver value{}; return packed(value.read(), 9, 11) - 4; }
)gti");
  expect(frontend.canGenerateCode(),
         "concrete entry observation fixture lowers");
  if (!frontend.canGenerateCode())
    return;
  using Query = lang::mir_detail::MirExistingPlaceQuery;
  using State = lang::mir_detail::MirDebugReadability;
  bool receiver = false;
  bool pack = false;
  for (const auto &hir : frontend.hir.functionInstances()) {
    if (!hir.source || (hir.source->name().lexeme != "read" &&
                        hir.source->name().lexeme != "packed"))
      continue;
    const bool member = hir.source->name().lexeme == "read";
    const auto *function = frontend.mir.findFunctionInstance(hir.id);
    const auto &body = function->body;
    const auto place = std::find_if(
        body.places.begin(), body.places.end(), [&](const auto &candidate) {
          return candidate.projections.empty() &&
                 (member ? candidate.root == lang::MirPlaceRootKind::This
                         : candidate.type.kind == lang::SemanticType::TypePack);
        });
    expect(place != body.places.end(),
           "query retains its exact receiver/pack root");
    if (place == body.places.end())
      continue;
    const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                       function->id};
    const std::vector<Query> requests{
        {place->id, {body.entry, 0, false, true}}};
    const auto withoutEntry = lang::mir_detail::verifyMirBodyWithPlaceQueries(
        body, function->id, requests);
    expect(withoutEntry.verification.valid() &&
               withoutEntry.requested.front().readability == State::Unknown,
           "a body alone cannot invent a receiver or pack entry contract");
    const auto exact = lang::mir_detail::verifyMirBodyWithPlaceQueries(
        frontend.mir, address, requests);
    expect(
        exact.verification.valid() &&
            exact.requested.front().readability == State::DefinitelyReadable,
        "exact concrete entry combines with canonical ownership/borrow flow");
    if (member) {
      const auto &locals = body.sourceDebug.tables->locals;
      const auto local =
          std::find_if(locals.begin(), locals.end(), [](const auto &row) {
            return row.declaration.receiver;
          });
      expect(local != locals.end() && local->place == place->id &&
                 local->declaration.name == "this" &&
                 local->declaration.declaration == lang::SourceSpan{.line = 0} &&
                 !local->declaration.binding && !local->declaration.symbol,
             "implicit receiver keeps accepted identity without a fake source "
             "token");
      if (local != locals.end()) {
        const std::vector<lang::MirDebugAvailabilityRequest> sourceRequests{
            {local->declaration.id, {body.entry, 0, false, true}}};
        const auto source = lang::queryMirDebugAvailability(
            frontend.mir, address, sourceRequests);
        expect(source.verification.valid() &&
                   source.requested.front().readability ==
                       State::DefinitelyReadable,
               "receiver source ranges use the canonical concrete entry "
               "observation");
        auto forged = body;
        forged.sourceDebug.tables->locals.at(local->declaration.id - 1)
            .declaration.receiver = false;
        expect(!lang::verifyMirBody(forged, function->id).valid(),
               "a receiver cannot forge an ordinary binding origin");
      }
      auto forged = frontend.hir;
      auto &rows = const_cast<std::vector<lang::HirFunctionInstance> &>(
          forged.functionInstances());
      for (auto &row : rows.at(hir.id - 1).body.sourceDebug.tables->locals)
        if (row.receiver)
          row.receiver = false;
      expect(!lang::verifyHirSourceDebug(frontend.semantics, forged).valid(),
             "HIR receiver origin must match the accepted semantic producer");
      auto semantic = frontend.semantics;
      auto &bodies = const_cast<std::vector<lang::SemanticSourceDebugBody> &>(
          semantic.sourceDebugBodies());
      for (auto &row : bodies)
        if (row.receiverType)
          row.receiverType = lang::SemanticType::Int32;
      expect(!semantic.verifySourceDebug().empty(),
             "semantic receiver cannot forge a different accepted owner type");
    }
    for (int mutation = 0; mutation < 4; ++mutation) {
      auto forged = frontend.mir;
      auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
          forged.functionInstances());
      auto &target = functions.at(function->id - 1);
      auto &storage = target.body.places.at(place->id - 1);
      if (mutation == 0) {
        if (member)
          target.owner.reset();
        else
          target.parameterTypes.back() = lang::SemanticType::Int64;
      } else if (mutation == 1) {
        if (member)
          target.staticMember = true;
        else
          storage.initiallyAvailable = false;
      } else if (mutation == 2) {
        storage.key.reset();
      } else {
        target.body.kind = lang::MirBodyKind::Module;
      }
      const auto rejected = lang::mir_detail::verifyMirBodyWithPlaceQueries(
          forged, address, requests);
      expect(!rejected.verification.valid() && rejected.requiredStops.empty() &&
                 rejected.requested.front().readability == State::Unknown,
             "forged formal/receiver identities cannot publish entry "
             "availability");
    }
    member ? receiver = true : pack = true;
  }
  expect(receiver && pack, "both concrete entry profiles were exercised");
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto entries = lang::mir_detail::debugEntryPlaces(
        frontend.mir, {lang::MirBodyKind::Constructor, constructor.id});
    expect(entries.initialized.empty(),
           "constructor storage is not a fully initialized receiver at entry");
  }
}

void testInitializerSourceDebugProducers() {
  const auto syntax =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Parsing})
          .analyze("source-debug-initializers.gti", R"gti(
mut int32_t trace = 0;
int32_t seed() { trace++; return trace; }
int32_t global = seed();
thread_local int32_t per_thread = seed();
struct Defaulted { int32_t value = seed(); };
struct StaticStorage { static int32_t value = seed(); };
struct Selected {
  int32_t value = seed();
  Selected() {}
  Selected(int32_t input) : value(input) {}
};
struct Generic<T> {
  int32_t value = seed();
};
struct EmptyPack<Ts...> { int32_t value = seed(); };
int main() {
  Defaulted first{};
  Selected second{};
  Selected third{3};
  Generic<int32_t> fourth{};
  Generic<int64_t> fifth{};
  EmptyPack<> sixth{};
  thread_local int32_t local = seed();
  return global + per_thread + first.value + second.value + third.value;
}
)gti");
  expect(syntax.syntaxValid, "initializer source fixture parses");
  if (!syntax.syntaxValid)
    return;
  lang::SemanticVisitor collected(
      lang::TargetInfo::host(), &syntax.sourceGraph, true, nullptr,
      &syntax.sources, lang::RawPointerAuditMode::Off, false,
      lang::TrustedBodyAnalysis::Complete, lang::SourceDebugProfile::LexicalV1);
  expect(collected.check(syntax.program),
         "initializer source fixture analyzes");
  expect(collected.applyProgramRequest(syntax.program, {}),
         "initializer source fixture applies its program request");
  for (const auto &diagnostic : collected.errors())
    std::cerr << "initializer semantic: " << diagnostic.message << '\n';
  for (const auto &issue : collected.model().verifySourceDebug())
    std::cerr << "initializer semantic debug: " << issue << '\n';
  expect(collected.model().verifySourceDebug().empty(),
         "initializer regions preserve accepted semantic identity");
  auto lowered = lang::HirLowerer(lang::TargetInfo::host())
                     .lower(syntax.program, collected.model(), collected);
  const auto verification =
      lang::verifyHirSourceDebug(collected.model(), lowered.program);
  for (const auto &issue : verification.errors)
    std::cerr << "initializer HIR debug: " << issue << '\n';
  expect(lowered.program.valid() && verification.valid(),
         "module, TLS, concrete fields and selected defaults retain regions");
  if (!lowered.program.valid() || !verification.valid())
    return;
  expect(lowered.program.module().sourceDebug.tables &&
             !lowered.program.module().sourceDebug.tables->initializers.empty(),
         "module source work has its own initializer regions");
  bool generated = false;
  bool selected = false;
  for (const auto &constructor : lowered.program.constructorInstances()) {
    const auto &record = constructor.body.sourceDebug;
    if (!record.tables || record.tables->initializers.empty())
      continue;
    generated |= constructor.generatedDefault;
    selected |= !constructor.generatedDefault;
    auto &body = const_cast<lang::HirBody &>(constructor.body);
    const auto saved = body.sourceDebug;
    body.sourceDebug.tables->initializers.front().roots = {0};
    expect(
        !lang::verifyHirSourceDebug(collected.model(), lowered.program).valid(),
        "foreign initializer roots cannot acquire source regions");
    body.sourceDebug = saved;
    body.sourceDebug.tables->initializers.clear();
    expect(
        !lang::verifyHirSourceDebug(collected.model(), lowered.program).valid(),
        "source defaults cannot be stripped from generated constructors");
    body.sourceDebug = saved;
  }
  expect(generated && selected,
         "generated and source constructors retain selected field defaults");
}

[[nodiscard]] bool
hasIssue(const std::vector<lang::LoweredProgramIssue> &issues,
         lang::LoweredProgramIssueKind kind) {
  return std::any_of(issues.begin(), issues.end(),
                     [kind](const lang::LoweredProgramIssue &issue) {
                       return issue.kind == kind;
                     });
}

[[nodiscard]] std::size_t generatedCount(const lang::LoweredProgram &program,
                                         lang::LoweredGeneratedItemKind kind) {
  return static_cast<std::size_t>(std::count_if(
      program.generatedItems().begin(), program.generatedItems().end(),
      [kind](const lang::LoweredGeneratedItem &item) {
        return item.identity.kind == kind;
      }));
}

template <typename Payload>
[[nodiscard]] const Payload *findPayload(const lang::LoweredProgram &program,
                                         std::string_view name = {}) {
  for (const lang::LoweredDeclaration &declaration : program.declarations()) {
    if (!name.empty() && declaration.name != name) {
      continue;
    }
    if (const auto *payload = std::get_if<Payload>(&declaration.payload)) {
      return payload;
    }
  }
  return nullptr;
}

[[nodiscard]] std::optional<lang::LoweredProgram>
buildDetachedProgram(std::string_view sourceName = "lowered-program.gti",
                     bool debugInfo = false) {
  lang::FrontendResult frontend =
      lang::Frontend(
          {.sourceDebugProfile = debugInfo ? lang::SourceDebugProfile::LexicalV1
                                           : lang::SourceDebugProfile::None})
          .analyze(sourceName, R"(
using Unary = (int32_t) -> int32_t;

enum class Mode : int32_t { active = 1, idle = 2 };

[[c_abi]] struct NativePair {
  int32_t left;
  int32_t right;
};

union Bits {
  int32_t signed_value;
  uint32_t unsigned_value;
};

class Counter {
public:
  mut int32_t value = 0;

  int32_t read() { return this.value; }
};

class Cursor {
  mut int32_t current = 0;
public:
  int32_t& operator*() { return this.current; }
  void operator++() mut { this.current++; }
  bool operator!=(Cursor& other) { return this.current != other.current; }
};

class Increment {
public:
  int32_t operator()(int32_t value) { return value + 1; }
};

class Cleanup {
  mut int32_t value = 1;
public:
  ~Cleanup() { this.value = 0; }
};

class GenericConstruction {
public:
  GenericConstruction<Left, Right>(Left left, Right right) {}
};

namespace demo {
class Marker<T> {
public:
  int32_t read() { return 1; }
};
}

class demo::Marker<int32_t> {
public:
  int32_t read() { return 2; }
};

mut int32_t process_seed = 1;

extern "C" {
  Unary install_callback(Unary callback);
}

int32_t add_one(int32_t value) { return value + 1; }

T identity<T>(T value) { return value; }

int32_t apply_offset(int32_t value) {
  int32_t offset = 1;
  auto add_offset = [offset](int32_t input) -> int32_t {
    return input + offset;
  };
  return add_offset(value);
}

int main() {
  unsafe {
    [[discard]] install_callback(add_one);
  }
  Counter counter = Counter();
  Cleanup cleanup = Cleanup();
  GenericConstruction generic = GenericConstruction(1, true);
  demo::Marker<int32_t> marker{};
  return identity<int32_t>(counter.read()) + process_seed + apply_offset(0) +
         marker.read() - 4;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    return std::nullopt;
  }

  lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O1,
                                        .target = lang::TargetInfo::host()});
  if (!optimized.valid()) {
    return std::nullopt;
  }
  lang::LoweredProgramBuild build = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, optimized.sourceMir,
      optimized.mir, optimized.report.transformations,
      lang::TargetInfo::host());
  if (!build.valid()) {
    for (const lang::LoweredProgramIssue &issue : build.issues) {
      std::cerr << "lowered-program issue: " << issue.detail << '\n';
    }
    return std::nullopt;
  }
  return std::move(build.program);
}

void testDetachedDeterministicProgram() {
  std::optional<lang::LoweredProgram> first = buildDetachedProgram();
  std::optional<lang::LoweredProgram> second = buildDetachedProgram();
  expect(first.has_value() && second.has_value(),
         "the builder should accept a rich optimized frontend program");
  if (!first || !second) {
    return;
  }

  const std::string firstText = lang::LoweredProgramPrinter().print(*first);
  const std::string secondText = lang::LoweredProgramPrinter().print(*second);
  expect(firstText == secondText,
         "independent frontend snapshots should lower deterministically");
  expect(firstText.find("lowered-program-v26") == 0 &&
             firstText.find("\nmir\n") != std::string::npos,
         "the deterministic printer should serialize the complete contract");
  const gti_test::LoweredProgramInventory inventory =
      gti_test::inspectLoweredProgram(*first);
  expect(inventory.deterministicText == firstText &&
             inventory.bodies == first->bodies().size() &&
             inventory.declarations == first->declarations().size() &&
             inventory.symbols == first->symbols().size() &&
             inventory.classInstances == first->classInstances().size() &&
             inventory.functionInstances == first->functionInstances().size() &&
             inventory.constructorInstances ==
                 first->constructorInstances().size() &&
             inventory.destructorInstances ==
                 first->destructorInstances().size() &&
             inventory.lambdaInstances == first->lambdaInstances().size() &&
             inventory.generatedItems == first->generatedItems().size() &&
             std::accumulate(inventory.declarationKinds.begin(),
                             inventory.declarationKinds.end(),
                             std::size_t{0}) == inventory.declarations &&
             std::accumulate(inventory.generatedItemKinds.begin(),
                             inventory.generatedItemKinds.end(),
                             std::size_t{0}) == inventory.generatedItems,
         "an independent contract client should enumerate and print the "
         "complete lowered program without frontend representations");
  expect(lang::verifyLoweredProgram(*first).empty(),
         "the detached lowered program should verify after frontend owners "
         "have been destroyed");
  expect(first->bodies().size() ==
             lang::enumerateMirBodyAddresses(first->mir()).size(),
         "the lowered body census should exactly match its owned MIR");
  const bool hasValueLayouts = std::any_of(
      first->mir().classInstances().begin(),
      first->mir().classInstances().end(),
      [](const lang::MirClassInstance &instance) {
        const bool validCAbi =
            instance.cAbiLayout &&
            std::all_of(instance.cAbiLayout->fields.begin(),
                        instance.cAbiLayout->fields.end(),
                        [](const lang::MirCAbiRecordFieldLayout &field) {
                          return field.field != 0;
                        });
        const bool validUnion =
            instance.unionLayout &&
            std::all_of(instance.unionLayout->fields.begin(),
                        instance.unionLayout->fields.end(),
                        [](const lang::MirUnionFieldLayout &field) {
                          return field.field != 0;
                        });
        return validCAbi || validUnion;
      });
  expect(hasValueLayouts,
         "MIR ABI and union layouts should retain symbol identities rather "
         "than AST declaration pointers");
  expect(
      generatedCount(
          *first, lang::LoweredGeneratedItemKind::ProgramInitialization) == 1 &&
          generatedCount(*first, lang::LoweredGeneratedItemKind::HostedEntry) ==
              1 &&
          generatedCount(
              *first, lang::LoweredGeneratedItemKind::NativeInteropAdapter) ==
              1 &&
          generatedCount(
              *first,
              lang::LoweredGeneratedItemKind::StructuralOperatorAdapter) == 3 &&
          generatedCount(
              *first, lang::LoweredGeneratedItemKind::CallableAdapter) == 1 &&
          generatedCount(
              *first, lang::LoweredGeneratedItemKind::LifecycleCleanup) == 1 &&
          generatedCount(
              *first,
              lang::LoweredGeneratedItemKind::ConcreteInstanceAdapter) == 2,
      "the lowered program should own startup, initialization, and native "
      "callback/declaration/lifecycle/concrete-instance adapter contracts "
      "without frontend pointers");
  const bool declarationAdaptersRooted = std::all_of(
      first->generatedItems().begin(), first->generatedItems().end(),
      [&](const lang::LoweredGeneratedItem &item) {
        if (item.identity.kind !=
                lang::LoweredGeneratedItemKind::StructuralOperatorAdapter &&
            item.identity.kind !=
                lang::LoweredGeneratedItemKind::CallableAdapter &&
            item.identity.kind !=
                lang::LoweredGeneratedItemKind::ConcreteInstanceAdapter) {
          return true;
        }
        const lang::LoweredDeclaration *source =
            first->findDeclaration(item.sourceDeclaration);
        return item.sourceKind ==
                   lang::LoweredGeneratedItemSourceKind::Declaration &&
               source != nullptr &&
               std::find(source->requiredGeneratedItems.begin(),
                         source->requiredGeneratedItems.end(),
                         item.identity) != source->requiredGeneratedItems.end();
      });
  expect(declarationAdaptersRooted,
         "structural, callable, and concrete-instance adapters should be "
         "rooted by their exact lowered declarations");
  const auto lifecycle = std::find_if(
      first->generatedItems().begin(), first->generatedItems().end(),
      [](const lang::LoweredGeneratedItem &item) {
        return item.identity.kind ==
               lang::LoweredGeneratedItemKind::LifecycleCleanup;
      });
  const auto *lifecyclePayload =
      lifecycle == first->generatedItems().end()
          ? nullptr
          : std::get_if<lang::LoweredLifecycleCleanupItem>(&lifecycle->payload);
  const lang::LoweredDeclaration *lifecycleSource =
      lifecycle == first->generatedItems().end()
          ? nullptr
          : first->findDeclaration(lifecycle->sourceDeclaration);
  expect(lifecyclePayload != nullptr && lifecycleSource != nullptr &&
             lifecycle->sourceKind ==
                 lang::LoweredGeneratedItemSourceKind::Declaration &&
             std::holds_alternative<lang::LoweredDestructorDeclaration>(
                 lifecycleSource->payload) &&
             lifecyclePayload->destructorInstance ==
                 lifecycle->identity.owner &&
             lifecyclePayload->form ==
                 lang::LoweredLifecycleCleanupForm::OrdinaryClass,
         "lifecycle cleanup should retain one exact destructor-rooted "
         "ordinary-class payload");
  const auto concrete = std::find_if(
      first->generatedItems().begin(), first->generatedItems().end(),
      [&](const lang::LoweredGeneratedItem &item) {
        const auto *payload =
            std::get_if<lang::LoweredConcreteInstanceAdapterItem>(
                &item.payload);
        const lang::LoweredDeclaration *source =
            first->findDeclaration(item.sourceDeclaration);
        return item.identity.kind ==
                   lang::LoweredGeneratedItemKind::ConcreteInstanceAdapter &&
               payload != nullptr &&
               payload->kind ==
                   lang::LoweredConcreteInstanceAdapterKind::Function &&
               source != nullptr && source->name == "identity";
      });
  const auto *concretePayload =
      concrete == first->generatedItems().end()
          ? nullptr
          : std::get_if<lang::LoweredConcreteInstanceAdapterItem>(
                &concrete->payload);
  const lang::LoweredDeclaration *concreteSource =
      concrete == first->generatedItems().end()
          ? nullptr
          : first->findDeclaration(concrete->sourceDeclaration);
  const lang::MirFunctionInstance *concreteMir =
      concretePayload == nullptr
          ? nullptr
          : first->mir().findFunctionInstance(concretePayload->body.owner);
  expect(concretePayload != nullptr && concreteSource != nullptr &&
             concreteMir != nullptr && concreteSource->name == "identity" &&
             concreteSource->generic &&
             concrete->sourceKind ==
                 lang::LoweredGeneratedItemSourceKind::Declaration &&
             concretePayload->kind ==
                 lang::LoweredConcreteInstanceAdapterKind::Function &&
             concretePayload->body.kind == lang::MirBodyKind::Function &&
             concretePayload->declaration == concreteMir->declaration &&
             concretePayload->mayRaiseDefinedFailure ==
                 concreteMir->mayRaiseDefinedFailure,
         "the concrete generic function row should preserve its exact "
         "declaration, MIR body, and failure effect");
  expect(!first->declarations().empty() &&
             std::all_of(first->declarations().begin(),
                         first->declarations().end(),
                         [](const lang::LoweredDeclaration &declaration) {
                           return declaration.id != 0;
                         }),
         "the active declaration census should use stable nonzero identities");

  const lang::LoweredClassDeclaration *nativePair =
      findPayload<lang::LoweredClassDeclaration>(*first, "NativePair");
  const lang::LoweredClassDeclaration *bits =
      findPayload<lang::LoweredClassDeclaration>(*first, "Bits");
  const lang::LoweredEnumDeclaration *mode =
      findPayload<lang::LoweredEnumDeclaration>(*first, "Mode");
  const lang::LoweredFunctionDeclaration *addOne =
      findPayload<lang::LoweredFunctionDeclaration>(*first, "add_one");
  const lang::LoweredStorageDeclaration *processSeed =
      findPayload<lang::LoweredStorageDeclaration>(*first, "process_seed");
  expect(
      nativePair != nullptr && nativePair->cAbiRecord &&
          nativePair->cAbiLayout.has_value() &&
          !nativePair->cAbiLayout->fields.empty() &&
          first->findClassDeclaration(nativePair->id) == nativePair,
      "class declarations should own resolved ABI layouts and stable lookup");
  expect(bits != nullptr && bits->kind == lang::ClassKind::Union &&
             bits->unionLayout.has_value() &&
             !bits->unionLayout->fields.empty() &&
             first->findClassDeclaration(bits->id) == bits,
         "union declarations should own resolved value layouts");
  expect(mode != nullptr && mode->enumerators.size() == 2 &&
             first->findEnumDeclaration(mode->id) == mode,
         "enum declarations should own resolved enumerator contracts");
  expect(addOne != nullptr && addOne->returnType == lang::SemanticType::Int32 &&
             addOne->parameters.size() == 1 &&
             addOne->parameters.front().symbol != 0 &&
             first->findFunctionDeclaration(addOne->id) == addOne,
         "function declarations should own resolved signatures and bindings");
  expect(processSeed != nullptr &&
             processSeed->type == lang::SemanticType::Int32 &&
             processSeed->hasInitializer &&
             first->findStorageDeclaration(processSeed->symbol) == processSeed,
         "storage declarations should own resolved type and initialization "
         "contracts");
  expect(!first->symbols().empty() &&
             first->findSymbol(processSeed->symbol) != nullptr &&
             first->findSymbol(processSeed->symbol)->name == "process_seed",
         "the lowered symbol vocabulary should resolve MIR symbol names "
         "without SemanticModel");
  expect(first->classInstances().size() ==
                 first->mir().classInstances().size() &&
             first->functionInstances().size() ==
                 first->mir().functionInstances().size() &&
             first->constructorInstances().size() ==
                 first->mir().constructorInstances().size() &&
             first->destructorInstances().size() ==
                 first->mir().destructorInstances().size() &&
             first->lambdaInstances().size() ==
                 first->mir().lambdaInstances().size() &&
             !first->lambdaInstances().empty() &&
             !first->lambdaInstances().front().captures.empty(),
         "lowered concrete-instance tables should exactly cover MIR and own "
         "lambda names");

  const lang::LoweredClassDeclaration *exactMarker = nullptr;
  const lang::LoweredDeclaration *exactMarkerRow = nullptr;
  for (const lang::LoweredDeclaration &declaration : first->declarations()) {
    const auto *candidate =
        std::get_if<lang::LoweredClassDeclaration>(&declaration.payload);
    if (candidate != nullptr && candidate->qualifiedName == "demo::Marker" &&
        candidate->exactSpecializationPrimary != 0) {
      exactMarker = candidate;
      exactMarkerRow = &declaration;
      break;
    }
  }
  expect(exactMarker != nullptr && exactMarkerRow != nullptr &&
             exactMarkerRow->namespaceScope ==
                 std::vector<std::string>{"demo"} &&
             exactMarker->exactTypeArguments ==
                 std::vector<lang::SemanticType>{lang::SemanticType::Int32} &&
             exactMarker->exactValueArguments.empty(),
         "the lowered declaration inventory should retain an exact class "
         "specialization's primary and canonical key");
}

void testBuilderRejectsForgedOptimizationEvidence() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("lowered-forged-optimization.gti", R"(
int main() {
  mut int value = 1;
  if (true) {
    value = 2;
  } else {
    value = 3;
  }
  return value;
}
)");
  const lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O1,
                                        .target = lang::TargetInfo::host()});
  expect(frontend.canGenerateCode() && optimized.valid() &&
             !optimized.report.transformations.empty(),
         "the forged-evidence fixture should produce verified transformations");
  if (!frontend.canGenerateCode() || !optimized.valid() ||
      optimized.report.transformations.empty()) {
    return;
  }

  std::vector<lang::MirTransformRecord> forged =
      optimized.report.transformations;
  forged.front().requiredAnalyses.dominance =
      !forged.front().requiredAnalyses.dominance;
  const lang::LoweredProgramBuild rejected =
      lang::LoweredProgramBuilder().build(
          frontend.program, frontend.semantics, frontend.hir,
          optimized.sourceMir, optimized.mir, forged, lang::TargetInfo::host());
  expect(!rejected.valid() &&
             hasIssue(rejected.issues,
                      lang::LoweredProgramIssueKind::InvalidOptimization),
         "LoweredProgramBuilder should independently reject forged optimizer "
         "evidence before sealing backend input");
}

void testDetachedDebugLocations() {
  auto normal = buildDetachedProgram();
  auto debug = buildDetachedProgram("debug-source.gti", true);
  expect(normal && debug,
         "debug capture should retain a valid detached program");
  if (!normal || !debug)
    return;
  for (const auto &body : normal->bodies()) {
    const auto *collected = debug->findBody(body.identity.address);
    expect(collected && collected->identity.placeDomain.body ==
                            body.identity.placeDomain.body,
           "debug collection must not change executable body domain numbering");
  }
  expect(std::all_of(
             normal->bodies().begin(), normal->bodies().end(),
             [](const auto &body) { return body.sourceLocations().empty(); }),
         "debug provenance should be opt-in");
  auto &bodies = lang::LoweredProgramTestAccess::bodies(*debug);
  const auto body =
      std::find_if(bodies.begin(), bodies.end(), [](const auto &body) {
        return !body.sourceLocations().empty();
      });
  expect(body != bodies.end(),
         "debug provenance should survive frontend destruction");
  if (body == bodies.end())
    return;
  expect(std::all_of(body->sourceLocations().begin(),
                     body->sourceLocations().end(),
                     [](const auto &row) {
                       return row.source.source.ends_with("debug-source.gti") &&
                              row.source.line > 0 &&
                              row.source.start < row.source.end;
                     }),
         "debug rows should own real source statement spans");
  body->sourceDebug.tables->locations.front().block = 999999;
  expect(hasIssue(lang::verifyLoweredProgram(*debug),
                  lang::LoweredProgramIssueKind::InvalidBodyInventory),
         "debug stages must identify exact optimized MIR operations");
}

void testDebugReceiverFragments() {
  const auto frontend =
      lang::Frontend(
          {.sourceDebugProfile = lang::SourceDebugProfile::LexicalV1})
          .analyze("receiver-fragments.gti", R"gti(
extern "C" { void checkpoint(); }
class Receiver {
public:
  int value = 4;
  int read() {
    int before = this.value;
    checkpoint();
    return this.value & before;
  }
};
int main() { Receiver owner{}; return owner.read(); }
)gti");
  expect(frontend.canGenerateCode(), "receiver fragment fixture lowers");
  if (!frontend.canGenerateCode())
    return;
  auto built = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, lang::TargetInfo::host());
  expect(built.valid(), "verified field loads produce lowered debug fragments");
  if (!built.valid())
    return;
  unsigned fragments = 0, beforeCall = 0, afterCall = 0, atCall = 0;
  for (const auto &body : built.program->bodies()) {
    if (!body.sourceDebug.tables)
      continue;
    const auto *mir =
        lang::findMirBody(built.program->mir(), body.identity.address);
    for (const auto &fragment : body.sourceDebug.tables->fragments) {
      ++fragments;
      const auto *place = mir->findPlace(fragment.place);
      const auto *value = mir->findValue(fragment.value);
      expect(place && value && value->info.type == place->type &&
                 place->projections.size() == 1 &&
                 place->projections.front().kind ==
                     lang::MirProjectionKind::Field,
             "fragment retains its exact typed field and existing load value");
      const std::vector<lang::MirDebugAvailabilityRequest> request{
          {fragment.local, fragment.begin}};
      const auto observed = lang::queryMirDebugAvailability(
          built.program->mir(), body.identity.address, request);
      expect(observed.verification.valid() &&
                 observed.requested.front().readability ==
                     lang::MirDebugReadability::DefinitelyReadable,
             "fragment begins inside canonical receiver availability");
      const auto &block = *mir->findBlock(fragment.begin.block);
      const auto position = [&](lang::MirDebugPoint point) {
        if (point.blockEntry)
          return std::size_t{0};
        for (std::size_t i = 0; i < block.instructions.size(); ++i)
          if (block.instructions[i].id == point.instruction)
            return 2 * i + 1 + point.after;
        return 2 * block.instructions.size() + 1;
      };
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Call) {
          const lang::MirDebugPoint boundary{block.id, instruction.id, true};
          if (position(fragment.begin) < position(boundary)) {
            ++beforeCall;
            const lang::MirDebugPoint call{block.id, instruction.id, false};
            atCall += position(fragment.begin) <= position(call) &&
                      position(call) < position(fragment.end);
            expect(position(fragment.end) <= position(boundary),
                   "field value does not survive an unknown memory writer");
          } else {
            ++afterCall;
          }
        }
    }
  }
  expect(fragments && beforeCall && afterCall && atCall,
         "field loads on both sides of the unknown writer were exercised");
  for (int mutation = 0; mutation != 4; ++mutation) {
    auto forged = *built.program;
    bool changed = false;
    for (auto &body : lang::LoweredProgramTestAccess::bodies(forged))
      if (body.sourceDebug.tables &&
          !body.sourceDebug.tables->fragments.empty()) {
        auto &fragment = body.sourceDebug.tables->fragments.front();
        if (mutation == 0)
          fragment.value = 0;
        else if (mutation == 1)
          fragment.place =
              body.sourceDebug.tables->locals.at(fragment.local - 1).place;
        else if (mutation == 2)
          fragment.end = {fragment.begin.block, 0, false};
        else
          body.sourceDebug.tables->fragments.clear();
        changed = true;
        break;
      }
    const auto issues = lang::verifyLoweredProgram(forged);
    expect(changed &&
               std::any_of(issues.begin(), issues.end(),
                           [](const auto &issue) {
                             return issue.detail.find("source fragments") !=
                                    std::string::npos;
                           }),
           "forged field, value, extent or missing fragments reject "
           "independently");
  }
}

void testDebugPackFragments() {
  const auto frontend =
      lang::Frontend(
          {.sourceDebugProfile = lang::SourceDebugProfile::LexicalV1})
          .analyze("debug-pack-fragments.gti", R"gti(
int first<Args...>(int input, Args... rest) { return input; }
int main() { return first(7) + first(7, 9, 11) == 14 ? 0 : 1; }
)gti");
  expect(frontend.canGenerateCode(), "pack fragment fixture lowers");
  if (!frontend.canGenerateCode())
    return;
  const auto built = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, lang::TargetInfo::host());
  expect(built.valid(), "pack parameter availability seals");
  if (!built.valid())
    return;
  unsigned fragments = 0;
  for (const auto &body : built.program->bodies())
    if (body.sourceDebug.tables)
      for (const auto &fragment : body.sourceDebug.tables->parameterFragments) {
        ++fragments;
        const auto &local =
            body.sourceDebug.tables->locals.at(fragment.local - 1);
        expect(
            local.declaration.parameter && local.place == fragment.place &&
                local.declaration.type.kind == lang::SemanticType::TypePack &&
                fragment.element < local.declaration.type.arguments.size(),
            "pack fragment binds exact source parameter and concrete element");
      }
  expect(fragments >= 2, "nonempty pack has fresh canonical availability");
  for (int mutation = 0; mutation != 5; ++mutation) {
    auto forged = *built.program;
    bool changed = false;
    for (auto &body : lang::LoweredProgramTestAccess::bodies(forged))
      if (body.sourceDebug.tables &&
          !body.sourceDebug.tables->parameterFragments.empty()) {
        auto &fragment = body.sourceDebug.tables->parameterFragments.front();
        if (mutation == 0)
          fragment.local = 0;
        else if (mutation == 1)
          fragment.place = 0;
        else if (mutation == 2)
          fragment.element = 99;
        else if (mutation == 3)
          fragment.end = fragment.begin;
        else
          body.sourceDebug.tables->parameterFragments.clear();
        changed = true;
        break;
      }
    const auto issues = lang::verifyLoweredProgram(forged);
    expect(
        changed && std::any_of(issues.begin(), issues.end(),
                               [](const auto &issue) {
                                 return issue.detail.find(
                                            "source parameter fragments") !=
                                        std::string::npos;
                               }),
        "forged pack origin, lane, extent and omission reject independently");
  }
}

void testDebugValueEvents() {
  const auto frontend =
      lang::Frontend(
          {.sourceDebugProfile = lang::SourceDebugProfile::LexicalV1})
          .analyze(
              "debug-values.gti",
              "int probe(int input) { mut int local = input & 31; local = "
              "local | 1; return local; } int main() { return probe(4); }");
  expect(frontend.canGenerateCode(), "debug value fixture analyzes");
  if (!frontend.canGenerateCode())
    return;
  unsigned controls = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto &body = function.body;
    if (!body.sourceDebug.tables ||
        body.sourceDebug.tables->valueEvents.empty())
      continue;
    const auto *hir = frontend.hir.findFunctionInstance(function.id);
    auto forged = body;
    forged.sourceDebug.tables->valueEvents.front().value = 0;
    expect(!lang::verifyMirBody(forged).valid(),
           "undefined debug SSA identity rejects");
    forged = body;
    forged.sourceDebug.tables->valueEvents.front().value =
        body.values.back().id;
    expect(!lang::verifyMirBody(forged).valid(),
           "future debug SSA definition rejects by canonical dominance");
    forged = body;
    forged.sourceDebug.tables->valueEvents.front().local =
        body.sourceDebug.tables->locals.back().declaration.id;
    expect(hir && !lang::mir_detail::sourceDebugMatchesHir(hir->body, forged),
           "a same-type source binding forgery fails canonical original-event "
           "reconstruction");
    const auto local = body.sourceDebug.tables->valueEvents.front().local;
    const auto place = body.sourceDebug.tables->locals[local - 1].place;
    const auto excluded = [&](const lang::MirBody &candidate) {
      const auto events =
          lang::mir_detail::deriveMirSourceValueEvents(candidate);
      return std::none_of(events.begin(), events.end(), [&](const auto &event) {
        return event.local == local;
      });
    };
    forged = body;
    const auto erased =
        body.sourceDebug.tables->valueEvents.front().point.instruction;
    auto undo = lang::mir_detail::repairMirSourceDebug(
        body, forged, {.removedInstructions = std::span(&erased, 1)});
    expect(
        std::none_of(forged.sourceDebug.tables->valueEvents.begin(),
                     forged.sourceDebug.tables->valueEvents.end(),
                     [&](const auto &event) { return event.local == local; }),
        "deleting a debug event anchor invalidates every value interval for "
        "that local");
    undo.restore(forged);
    expect(
        forged == body,
        "refused optimization restores all debug value events in exact order");
    forged = body;
    forged.blocks.front().terminator.targetArguments.push_back(
        {.kind = lang::MirOperandKind::Copy, .place = place});
    expect(excluded(forged), "place-bearing edge transport cannot acquire "
                             "scalar SSA debug authority");
    for (const auto kind :
         {lang::MirOperandKind::Move, lang::MirOperandKind::Address}) {
      forged = body;
      bool changed = false;
      for (auto &block : forged.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.kind == lang::MirInstructionKind::Load &&
              instruction.operands.size() == 1 &&
              instruction.operands.front().place == place) {
            instruction.operands.front().kind = kind;
            changed = true;
          }
      expect(changed && excluded(forged), "moves and escaping addresses retire "
                                          "all inferred scalar value events");
    }
    forged = body;
    const auto *storage = body.findPlace(place);
    expect(storage && storage->key.has_value(),
           "source scalar has a verified ownership key");
    if (storage && storage->key) {
      forged.blocks.front().instructions.front().ownership =
          lang::OwnershipEvent{.kind = lang::OwnershipEventKind::Move,
                               .place = *storage->key};
      expect(excluded(forged),
             "ownership moves cannot be bypassed by stale scalar SSA values");
    }
    ++controls;
  }
  const auto optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O3,
                                        .target = lang::TargetInfo::host()});
  expect(optimized.valid(), "debug events preserve optimizer eligibility");
  auto built = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, optimized.sourceMir,
      optimized.mir, optimized.report.transformations,
      lang::TargetInfo::host());
  expect(built.valid(), "verified promotion seals debug value events");
  if (!built.valid())
    return;
  unsigned promoted = 0;
  for (const auto &body : built.program->bodies())
    if (body.sourceDebug.tables)
      for (const auto &local : body.sourceDebug.tables->locals)
        if (local.declaration.name == "local" &&
            local.policy == lang::MirSourceLocalPolicy::OptimizedOut) {
          expect(std::any_of(body.sourceDebug.tables->valueRanges.begin(),
                             body.sourceDebug.tables->valueRanges.end(),
                             [&](const auto &range) {
                               return range.local == local.declaration.id;
                             }),
                 "promoted-away storage retains a verified SSA value range");
          ++promoted;
        }
  expect(controls && promoted,
         "SSA event positive and rejection controls executed");
  auto forged = *built.program;
  for (auto &body : lang::LoweredProgramTestAccess::bodies(forged))
    if (body.sourceDebug.tables &&
        !body.sourceDebug.tables->valueRanges.empty()) {
      body.sourceDebug.tables->valueRanges.front().value = 0;
      const auto issues = lang::verifyLoweredProgram(forged);
      expect(std::any_of(issues.begin(), issues.end(),
                         [](const auto &issue) {
                           return issue.detail.find(
                                      "verified MIR value events") !=
                                  std::string::npos;
                         }),
             "lowered debug value forgery rejects independently of the seal");
      break;
    }
}

void testDetachedDebugRanges() {
  const auto frontend =
      lang::Frontend(
          {.sourceDebugProfile = lang::SourceDebugProfile::LexicalV1})
          .analyze("debug-ranges.gti", R"gti(
int probe(int input) {
  mut int result = input;
  { int local = input + 1; result = local; }
  { int local = input + 2; result = result + local; }
  return result;
}
int main() { return probe(3); }
)gti");
  expect(frontend.canGenerateCode(), "lexical-range fixture must analyze");
  if (!frontend.canGenerateCode())
    return;
  auto built = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, lang::TargetInfo::host());
  expect(built.valid(),
         "fresh scalar availability must construct lowered ranges");
  if (!built.valid())
    return;
  std::size_t ranges = 0;
  std::size_t shadows = 0;
  for (const auto &body : built.program->bodies()) {
    if (!body.sourceDebug.tables)
      continue;
    const auto &tables = *body.sourceDebug.tables;
    for (const auto &local : tables.locals)
      if (local.declaration.name == "local") {
        ++shadows;
        expect(std::any_of(tables.ranges.begin(), tables.ranges.end(),
                           [&](const auto &range) {
                             return range.local == local.declaration.id &&
                                    range.place == local.place;
                           }),
               "each shadowed declaration must retain its own existing-place "
               "interval");
      }
    std::vector<lang::MirDebugAvailabilityRequest> requests;
    for (const auto &range : tables.ranges) {
      ++ranges;
      expect(range.begin.block == range.end.block && range.begin != range.end,
             "source ranges are nonempty and block-local");
      requests.push_back({range.local, range.begin});
    }
    const auto observed = lang::queryMirDebugAvailability(
        built.program->mir(), body.identity.address, requests);
    expect(
        observed.verification.valid() &&
            std::all_of(observed.requested.begin(), observed.requested.end(),
                        [](const auto &fact) {
                          return fact.readability ==
                                 lang::MirDebugReadability::DefinitelyReadable;
                        }),
        "every range begins with fresh definitely-readable existing storage");
  }
  expect(ranges != 0 && shadows == 2 &&
             lang::verifyLoweredProgram(*built.program).empty(),
         "sealed range inventory retains distinct lexical locals");
  auto forged = *built.program;
  for (auto &body : lang::LoweredProgramTestAccess::bodies(forged))
    if (body.sourceDebug.tables && !body.sourceDebug.tables->ranges.empty()) {
      body.sourceDebug.tables->ranges.front().end =
          body.sourceDebug.tables->ranges.front().begin;
      const auto issues = lang::verifyLoweredProgram(forged);
      expect(std::any_of(issues.begin(), issues.end(),
                         [](const auto &issue) {
                           return issue.detail.find(
                                      "fresh current MIR availability") !=
                                  std::string::npos;
                         }),
             "range mutations must fail independent fresh recomputation, not "
             "only the seal");
      expect(lang::verifyLoweredProgram(*built.program).empty(),
             "range table copies must not alias the accepted snapshot");
      break;
    }
}

void testVerificationEvidenceOwnsItsProgram() {
  auto source = buildDetachedProgram();
  expect(source.has_value(), "verification evidence fixture should lower");
  if (!source)
    return;
  std::vector<lang::LoweredProgramIssue> issues;
  const auto verified = lang::verifiedLoweredProgram(*source, issues);
  expect(verified.has_value() && issues.empty(),
         "a valid program should produce verification evidence");
  if (!verified)
    return;
  // Public value assignment is legal even though individual rows are exposed
  // read-only. It must not change the value a consumer has already verified.
  auto invalid = *source;
  auto &declarations = lang::LoweredProgramTestAccess::declarations(invalid);
  expect(!declarations.empty(), "evidence fixture needs declarations");
  if (declarations.empty())
    return;
  declarations.front().name += "_unverified_replacement";
  *source = std::move(invalid);
  expect(!lang::verifyLoweredProgram(*source).empty(),
         "the replacement must invalidate the original construction seal");
  expect(lang::verifyLoweredProgram(verified->program()).empty(),
         "verification evidence must retain its exact accepted value across "
         "public assignment of the source object");
  expect(!lang::verifiedLoweredProgram(*source, issues).has_value() &&
             !issues.empty(),
         "a newly acquired proof must reject the invalid replacement");
  source.reset();
  auto shared = *verified;
  auto transferred = std::move(shared);
  expect(&transferred.program() == &verified->program() &&
             &shared.program() == &verified->program(),
         "evidence copies and transfers share one live immutable snapshot");
  expect(lang::verifyLoweredProgram(transferred.program()).empty(),
         "the snapshot must outlive the original source object");

  auto owned = buildDetachedProgram();
  expect(owned.has_value(), "the owning handoff fixture should lower");
  if (!owned)
    return;
  const auto *rows = owned->declarations().data();
  const auto accepted = lang::verifiedLoweredProgram(std::move(*owned), issues);
  expect(accepted.has_value() && issues.empty(),
         "a moved valid value should produce verification evidence");
  if (accepted) {
    expect(accepted->program().declarations().data() == rows,
           "owning handoff must preserve the declaration storage without "
           "copying the complete lowered program");
  }
}

void testMutationRejection() {
  std::optional<lang::LoweredProgram> built = buildDetachedProgram();
  expect(built.has_value(),
         "the mutation fixture should produce a valid lowered program");
  if (!built) {
    return;
  }
  const lang::LoweredProgram original = *built;

  lang::LoweredProgram missing = original;
  lang::LoweredProgramTestAccess::generatedItems(missing).pop_back();
  expect(hasIssue(lang::verifyLoweredProgram(missing),
                  lang::LoweredProgramIssueKind::InvalidGeneratedItemInventory),
         "deleting a generated item should invalidate the exact inventory");

  lang::LoweredProgram duplicated = original;
  auto &duplicatedItems =
      lang::LoweredProgramTestAccess::generatedItems(duplicated);
  duplicatedItems.push_back(duplicatedItems.front());
  expect(hasIssue(lang::verifyLoweredProgram(duplicated),
                  lang::LoweredProgramIssueKind::DuplicateGeneratedItem),
         "duplicating a generated identity should be diagnosed");

  lang::LoweredProgram reordered = original;
  auto &reorderedItems =
      lang::LoweredProgramTestAccess::generatedItems(reordered);
  std::reverse(reorderedItems.begin(), reorderedItems.end());
  expect(hasIssue(lang::verifyLoweredProgram(reordered),
                  lang::LoweredProgramIssueKind::InvalidGeneratedItemInventory),
         "reordering generated contracts should invalidate deterministic "
         "construction");

  lang::LoweredProgram derooted = original;
  for (lang::LoweredBody &body :
       lang::LoweredProgramTestAccess::bodies(derooted)) {
    body.requiredGeneratedItems.clear();
  }
  const std::vector<lang::LoweredProgramIssue> derootedIssues =
      lang::verifyLoweredProgram(derooted);
  expect(hasIssue(derootedIssues,
                  lang::LoweredProgramIssueKind::OrphanGeneratedItem),
         "de-rooting generated contracts should expose orphan items");

  lang::LoweredProgram declarationDerooted = original;
  for (lang::LoweredDeclaration &declaration :
       lang::LoweredProgramTestAccess::declarations(declarationDerooted)) {
    declaration.requiredGeneratedItems.clear();
  }
  const std::vector<lang::LoweredProgramIssue> declarationDerootedIssues =
      lang::verifyLoweredProgram(declarationDerooted);
  expect(
      hasIssue(declarationDerootedIssues,
               lang::LoweredProgramIssueKind::InvalidGeneratedItemInventory) &&
          hasIssue(declarationDerootedIssues,
                   lang::LoweredProgramIssueKind::OrphanGeneratedItem),
      "de-rooting declaration adapters should fail the exact inventory and "
      "reachability checks");

  lang::LoweredProgram cyclic = original;
  auto &cyclicItems = lang::LoweredProgramTestAccess::generatedItems(cyclic);
  expect(!cyclicItems.empty(),
         "the mutation fixture should contain generated items");
  if (!cyclicItems.empty()) {
    cyclicItems.front().dependencies = {cyclicItems.front().identity};
    expect(
        hasIssue(lang::verifyLoweredProgram(cyclic),
                 lang::LoweredProgramIssueKind::CyclicGeneratedItemDependency),
        "a generated-item dependency cycle should be diagnosed");
  }

  lang::LoweredProgram invalidLifecycle = original;
  auto &lifecycleItems =
      lang::LoweredProgramTestAccess::generatedItems(invalidLifecycle);
  const auto lifecycle =
      std::find_if(lifecycleItems.begin(), lifecycleItems.end(),
                   [](const lang::LoweredGeneratedItem &item) {
                     return item.identity.kind ==
                            lang::LoweredGeneratedItemKind::LifecycleCleanup;
                   });
  expect(lifecycle != lifecycleItems.end(),
         "the mutation fixture should contain lifecycle cleanup");
  if (lifecycle != lifecycleItems.end()) {
    auto &payload =
        std::get<lang::LoweredLifecycleCleanupItem>(lifecycle->payload);
    ++payload.classInstance;
    expect(
        hasIssue(lang::verifyLoweredProgram(invalidLifecycle),
                 lang::LoweredProgramIssueKind::InvalidGeneratedItemInventory),
        "forging a lifecycle class instance should invalidate its exact "
        "generated-item contract");
  }

  lang::LoweredProgram invalidConcrete = original;
  auto &concreteItems =
      lang::LoweredProgramTestAccess::generatedItems(invalidConcrete);
  const auto concrete = std::find_if(
      concreteItems.begin(), concreteItems.end(),
      [](const lang::LoweredGeneratedItem &item) {
        return item.identity.kind ==
               lang::LoweredGeneratedItemKind::ConcreteInstanceAdapter;
      });
  expect(concrete != concreteItems.end(),
         "the mutation fixture should contain a concrete-instance adapter");
  if (concrete != concreteItems.end()) {
    auto &payload =
        std::get<lang::LoweredConcreteInstanceAdapterItem>(concrete->payload);
    ++payload.declaration;
    expect(
        hasIssue(lang::verifyLoweredProgram(invalidConcrete),
                 lang::LoweredProgramIssueKind::InvalidGeneratedItemInventory),
        "forging a concrete-instance declaration should invalidate its exact "
        "generated-item contract");
  }

  lang::LoweredProgram staleSeal = original;
  auto &declarations = lang::LoweredProgramTestAccess::declarations(staleSeal);
  expect(!declarations.empty(),
         "the mutation fixture should contain declarations");
  if (!declarations.empty()) {
    declarations.front().name += "_forged";
    expect(hasIssue(lang::verifyLoweredProgram(staleSeal),
                    lang::LoweredProgramIssueKind::InvalidConstructionSeal),
           "mutating pointer-free payload should invalidate the construction "
           "seal");
  }

  lang::LoweredProgram invalidPayload = original;
  auto &invalidDeclarations =
      lang::LoweredProgramTestAccess::declarations(invalidPayload);
  const auto function = std::find_if(
      invalidDeclarations.begin(), invalidDeclarations.end(),
      [](const lang::LoweredDeclaration &declaration) {
        return std::holds_alternative<lang::LoweredFunctionDeclaration>(
            declaration.payload);
      });
  expect(function != invalidDeclarations.end(),
         "the mutation fixture should contain a function payload");
  if (function != invalidDeclarations.end()) {
    std::get<lang::LoweredFunctionDeclaration>(function->payload).returnType =
        lang::SemanticType::Unknown;
    const std::vector<lang::LoweredProgramIssue> payloadIssues =
        lang::verifyLoweredProgram(invalidPayload);
    expect(hasIssue(payloadIssues,
                    lang::LoweredProgramIssueKind::InvalidDeclarationInventory),
           "an unresolved declaration payload should be rejected");
    expect(hasIssue(payloadIssues,
                    lang::LoweredProgramIssueKind::InvalidConstructionSeal),
           "declaration payload mutation should invalidate the construction "
           "seal");
  }

  lang::LoweredProgram duplicateIdentity = original;
  lang::LoweredProgram missingSourceIdentity = original;
  for (auto &declaration :
       lang::LoweredProgramTestAccess::declarations(missingSourceIdentity)) {
    if (auto *payload =
            std::get_if<lang::LoweredClassDeclaration>(&declaration.payload)) {
      payload->sourceUnit = 999999;
      break;
    }
  }
  const auto sourceIssues = lang::verifyLoweredProgram(missingSourceIdentity);
  expect(std::any_of(
             sourceIssues.begin(), sourceIssues.end(),
             [](const auto &issue) {
               return issue.kind == lang::LoweredProgramIssueKind::
                                        InvalidDeclarationInventory &&
                      issue.detail ==
                          "lowered declaration has no logical source identity";
             }),
         "a declaration without a logical source identity must fail before "
         "emission");
  auto &duplicateDeclarations =
      lang::LoweredProgramTestAccess::declarations(duplicateIdentity);
  lang::LoweredFunctionDeclaration *firstFunction = nullptr;
  lang::LoweredDeclaration *secondFunctionRow = nullptr;
  for (lang::LoweredDeclaration &declaration : duplicateDeclarations) {
    auto *payload =
        std::get_if<lang::LoweredFunctionDeclaration>(&declaration.payload);
    if (payload == nullptr) {
      continue;
    }
    if (firstFunction == nullptr) {
      firstFunction = payload;
    } else {
      secondFunctionRow = &declaration;
      break;
    }
  }

  lang::LoweredProgram invalidSpecialization = original;
  auto &specializationDeclarations =
      lang::LoweredProgramTestAccess::declarations(invalidSpecialization);
  const auto exactSpecialization = std::find_if(
      specializationDeclarations.begin(), specializationDeclarations.end(),
      [](const lang::LoweredDeclaration &declaration) {
        const auto *payload =
            std::get_if<lang::LoweredClassDeclaration>(&declaration.payload);
        return payload != nullptr && payload->exactSpecializationPrimary != 0;
      });
  expect(exactSpecialization != specializationDeclarations.end(),
         "the mutation fixture should contain an exact specialization");
  if (exactSpecialization != specializationDeclarations.end()) {
    const std::size_t exactIndex = static_cast<std::size_t>(
        std::distance(specializationDeclarations.begin(), exactSpecialization));
    std::get<lang::LoweredClassDeclaration>(exactSpecialization->payload)
        .exactSpecializationPrimary = 999999;
    expect(hasIssue(lang::verifyLoweredProgram(invalidSpecialization),
                    lang::LoweredProgramIssueKind::InvalidDeclarationInventory),
           "an exact specialization with a missing primary should be "
           "rejected");

    lang::LoweredProgram wrongSpecializationScope = original;
    lang::LoweredProgramTestAccess::declarations(
        wrongSpecializationScope)[exactIndex]
        .namespaceScope = {"wrong"};
    expect(hasIssue(lang::verifyLoweredProgram(wrongSpecializationScope),
                    lang::LoweredProgramIssueKind::InvalidDeclarationInventory),
           "an exact specialization outside its primary's canonical namespace "
           "should be rejected");
  }
  expect(firstFunction != nullptr && secondFunctionRow != nullptr,
         "the mutation fixture should contain multiple function identities");
  if (firstFunction != nullptr && secondFunctionRow != nullptr) {
    auto &secondFunction =
        std::get<lang::LoweredFunctionDeclaration>(secondFunctionRow->payload);
    secondFunction.id = firstFunction->id;
    secondFunctionRow->semanticIdentity = firstFunction->id;
    expect(hasIssue(lang::verifyLoweredProgram(duplicateIdentity),
                    lang::LoweredProgramIssueKind::InvalidDeclarationInventory),
           "duplicate semantic declaration identities should be rejected");
  }

  lang::LoweredProgram missingInstance = original;
  auto &functionInstances =
      lang::LoweredProgramTestAccess::functionInstances(missingInstance);
  expect(!functionInstances.empty(),
         "the mutation fixture should contain concrete function instances");
  if (!functionInstances.empty()) {
    functionInstances.pop_back();
    expect(hasIssue(lang::verifyLoweredProgram(missingInstance),
                    lang::LoweredProgramIssueKind::InvalidInstanceInventory),
           "deleting concrete-instance metadata should be rejected");
  }

  lang::LoweredProgram forgedConstructorArgument = original;
  lang::LoweredProgram forgedConstructorConstexpr = original;
  auto &constantMir =
      lang::LoweredProgramTestAccess::mir(forgedConstructorConstexpr);
  auto &constantConstructors =
      const_cast<std::vector<lang::MirConstructorInstance> &>(
          constantMir.constructorInstances());
  expect(!constantConstructors.empty(),
         "fixture has constructor eligibility metadata");
  if (!constantConstructors.empty()) {
    constantConstructors.front().constexprConstructor =
        !constantConstructors.front().constexprConstructor;
    expect(hasIssue(lang::verifyLoweredProgram(forgedConstructorConstexpr),
                    lang::LoweredProgramIssueKind::InvalidInstanceInventory),
           "constructor constexpr eligibility cannot diverge from its semantic "
           "declaration");
  }
  auto &constructorInstances =
      lang::LoweredProgramTestAccess::constructorInstances(
          forgedConstructorArgument);
  const auto genericConstructor =
      std::find_if(constructorInstances.begin(), constructorInstances.end(),
                   [](const lang::LoweredConstructorInstance &instance) {
                     return instance.typeArguments.size() == 2;
                   });
  expect(genericConstructor != constructorInstances.end(),
         "the mutation fixture should contain a constructor-local generic "
         "instance");
  if (genericConstructor != constructorInstances.end()) {
    std::swap(genericConstructor->typeArguments[0],
              genericConstructor->typeArguments[1]);
    expect(hasIssue(lang::verifyLoweredProgram(forgedConstructorArgument),
                    lang::LoweredProgramIssueKind::InvalidInstanceInventory),
           "reordering constructor-local type arguments without changing "
           "the MIR identity should be rejected");

    lang::LoweredProgram forgedConstructorParameter = original;
    lang::MirProgram &mir =
        lang::LoweredProgramTestAccess::mir(forgedConstructorParameter);
    auto &mirConstructors =
        const_cast<std::vector<lang::MirConstructorInstance> &>(
            mir.constructorInstances());
    const auto concrete =
        std::find_if(mirConstructors.begin(), mirConstructors.end(),
                     [](const lang::MirConstructorInstance &instance) {
                       return instance.typeArguments.size() == 2;
                     });
    if (concrete != mirConstructors.end() &&
        !concrete->parameterTypes.empty()) {
      concrete->parameterTypes.front() = lang::SemanticType::Bool;
    }
    expect(
        concrete != mirConstructors.end() &&
            !concrete->parameterTypes.empty() &&
            hasIssue(lang::verifyLoweredProgram(forgedConstructorParameter),
                     lang::LoweredProgramIssueKind::InvalidInstanceInventory),
        "substituting a constructor parameter type without changing its "
        "concrete generic identity should be rejected");
  }

  lang::LoweredProgram duplicateSymbol = original;
  auto &symbols = lang::LoweredProgramTestAccess::symbols(duplicateSymbol);
  expect(symbols.size() > 1,
         "the mutation fixture should contain multiple lowered symbols");
  if (symbols.size() > 1) {
    symbols[1].id = symbols.front().id;
    expect(hasIssue(lang::verifyLoweredProgram(duplicateSymbol),
                    lang::LoweredProgramIssueKind::InvalidSymbolInventory),
           "duplicate lowered symbol identities should be rejected");
  }

  // Static method invariants are structural, not an exception for one owner
  // kind. Exercise both a normal class and an unrelated interface factory.
  auto staticFrontend = lang::Frontend().analyze("static-method-contracts.gti",
                                                 R"gti(
class Factory {
public:
  static int Build(int value) { return value; }
};
interface FactoryApi {
  static int Select(int value) { return Factory::Build(value); }
  int value();
};
class Signal : public FactoryApi {
public:
  int value() override { return 3; }
};
int main() {
  Signal signal{};
  return Factory::Build(1) + FactoryApi::Select(2) + signal.value() - 6;
}
)gti");
  expect(staticFrontend.canGenerateCode(),
         "class and interface static methods share ordinary executable rules");
  if (!staticFrontend.canGenerateCode()) {
    for (const auto &diagnostic : staticFrontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  auto staticBuild = lang::LoweredProgramBuilder().build(
      staticFrontend.program, staticFrontend.semantics, staticFrontend.hir,
      staticFrontend.mir, staticFrontend.mir, {}, lang::TargetInfo::host());
  expect(staticBuild.valid(),
         "class and interface static contracts cross the lowered boundary");
  if (!staticBuild.valid()) {
    for (const auto &issue : staticBuild.issues)
      std::cerr << issue.detail << '\n';
    return;
  }
  const lang::LoweredProgram staticOriginal = *staticBuild.program;
  std::size_t staticTargets = 0;
  for (const auto &candidate : staticOriginal.mir().functionInstances()) {
    if (!candidate.staticMember)
      continue;
    ++staticTargets;
    expect(
        candidate.owner && !candidate.virtualMethod && !candidate.pureVirtual &&
            !candidate.overrideMethod && candidate.virtualRoots.empty() &&
            candidate.receiverMutability == lang::ReceiverMutability::ReadOnly,
        "static methods preserve an owner without a receiver or dispatch "
        "contract");
    const std::size_t functionIndex = candidate.id - 1;
    for (int mutation = 0; mutation < 8; ++mutation) {
      lang::MirProgram forged = staticOriginal.mir();
      auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
          forged.functionInstances());
      auto &function = functions.at(functionIndex);
      switch (mutation) {
      case 0:
        function.virtualMethod = true;
        break;
      case 1:
        function.pureVirtual = true;
        break;
      case 2:
        function.overrideMethod = true;
        break;
      case 3:
        function.virtualRoots.push_back(function.declaration);
        break;
      case 4:
        function.receiverMutability = lang::ReceiverMutability::Mutable;
        break;
      case 5:
        function.receiverMutability = lang::ReceiverMutability::Consuming;
        break;
      case 6:
        function.owner.reset();
        break;
      case 7: {
        lang::MirPlace receiver;
        receiver.id = function.body.places.size() + 1;
        receiver.root = lang::MirPlaceRootKind::This;
        receiver.type = forged.findClassInstance(*candidate.owner)->type;
        function.body.places.push_back(std::move(receiver));
        break;
      }
      }
      const auto verification = lang::verifyMirProgram(forged);
      expect(!verification.valid() &&
                 std::any_of(
                     verification.errors.begin(), verification.errors.end(),
                     [&](const lang::MirVerificationError &error) {
                       return error.owner == candidate.id &&
                              error.message.find("static method must") !=
                                  std::string::npos;
                     }),
             "MIR must reject forged static dispatch, receiver or owner "
             "metadata for every owner kind");
    }
    for (int mutation = 0; mutation < 6; ++mutation) {
      lang::LoweredProgram forged = staticOriginal;
      auto &declarations = lang::LoweredProgramTestAccess::declarations(forged);
      const auto row =
          std::find_if(declarations.begin(), declarations.end(),
                       [&](const lang::LoweredDeclaration &declaration) {
                         const auto *payload =
                             std::get_if<lang::LoweredFunctionDeclaration>(
                                 &declaration.payload);
                         return payload && payload->id == candidate.declaration;
                       });
      expect(row != declarations.end(),
             "static method has one exact lowered declaration");
      if (row == declarations.end())
        continue;
      auto &payload = std::get<lang::LoweredFunctionDeclaration>(row->payload);
      switch (mutation) {
      case 0:
        payload.staticMember = false;
        break;
      case 1:
        payload.virtualMethod = true;
        break;
      case 2:
        payload.pureVirtual = true;
        break;
      case 3:
        payload.overrideMethod = true;
        break;
      case 4:
        payload.virtualRoots.push_back(payload.id);
        break;
      case 5:
        payload.receiverMutability = lang::ReceiverMutability::Mutable;
        break;
      }
      expect(hasIssue(lang::verifyLoweredProgram(forged),
                      lang::LoweredProgramIssueKind::InvalidInstanceInventory),
             "lowered signatures cannot disagree with static-method MIR "
             "dispatch or receiver authority");
    }
  }
  expect(staticTargets == 2,
         "mutations cover both class and interface static method instances");
}

void testConcreteBranchSymbols() {
  constexpr std::string_view source = R"gti(
T select<T, uint64_t Mode>(T value) {
  if constexpr (Mode == 0) {
    T local = value;
    { T local = value; }
    return local;
  } else {
    T alternative = value;
    return alternative;
  }
}
int main() {
  return select<int, 0>(7) == 7 && select<bool, 0>(true) &&
         select<int, 1>(9) == 9 ? 0 : 1;
}
)gti";
  auto frontend = lang::Frontend().analyze("concrete-branch-symbols.gti",
                                           std::string(source));
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "selected branches retain semantic locals");
  if (!frontend.canGenerateCode())
    return;
  const auto &symbols = frontend.hir.instanceSymbols();
  expect(symbols.size() == 5 &&
             std::count_if(symbols.begin(), symbols.end(),
                           [](const auto &s) {
                             return s.name == "local" &&
                                    s.type == lang::SemanticType::Bool;
                           }) == 2 &&
             std::count_if(symbols.begin(), symbols.end(),
                           [](const auto &s) {
                             return s.name == "local" &&
                                    s.type == lang::SemanticType::Int32;
                           }) == 2,
         "two instances and shadowed locals retain separate typed identities");
  auto built = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, lang::TargetInfo::host());
  expect(built.valid(), "concrete-only locals cross the lowered inventory");
  if (!built.valid())
    return;
  for (const auto &symbol : symbols) {
    const auto *lowered = built.program->findSymbol(symbol.id);
    expect(lowered && lowered->type == symbol.type &&
               lowered->nameSource == symbol.nameSpan,
           "concrete symbol type and source identity survive lowering");
  }
  auto missing = *built.program;
  std::erase_if(lang::LoweredProgramTestAccess::symbols(missing),
                [&](const auto &symbol) { return symbol.id == symbols[0].id; });
  expect(hasIssue(lang::verifyLoweredProgram(missing),
                  lang::LoweredProgramIssueKind::InvalidSymbolInventory),
         "missing selected-arm symbol still fails verification");
  auto wrong = *built.program;
  auto &wrongSymbols = lang::LoweredProgramTestAccess::symbols(wrong);
  const auto record = std::find_if(
      wrongSymbols.begin(), wrongSymbols.end(),
      [&](const auto &symbol) { return symbol.id == symbols[0].id; });
  record->type = lang::SemanticType::Unknown;
  expect(hasIssue(lang::verifyLoweredProgram(wrong),
                  lang::LoweredProgramIssueKind::InvalidSymbolInventory),
         "a forged unresolved concrete local fails verification");
  auto inactive = lang::Frontend().analyze("inactive-branch.gti", R"gti(
int choose<uint64_t N>() {
  if constexpr (N == 0) { int local = 2; return local; }
  else { Missing type{}; return unknown_name; }
}
int main() { return choose<0>() - 2; }
)gti");
  expect(inactive.canGenerateCode(), "discarded arms remain unanalyzed");
  auto invalidBorrow =
      lang::Frontend().analyze("selected-borrow-escape.gti", R"gti(
int& borrow<uint64_t N>(int& source) {
  if constexpr (N == 0) { int local = 2; return local; }
}
int main() { int value = 1; return borrow<0>(value); }
)gti");
  expect(!invalidBorrow.canGenerateCode() &&
             std::any_of(invalidBorrow.diagnostics.begin(),
                         invalidBorrow.diagnostics.end(),
                         [](const auto &d) { return d.code == "GTI-S2017"; }),
         "selected-arm locals still cannot escape by reference");
  auto forged = frontend.hir;
  auto &forgedSymbols =
      const_cast<std::vector<lang::SymbolRecord> &>(forged.instanceSymbols());
  forgedSymbols.front().type = lang::SemanticType::Bool;
  expect(!lang::verifyHirProgramPlans(frontend.semantics, forged).valid(),
         "a forged concrete symbol type fails HIR provenance verification");
}

void testConcreteLambdaPacks() {
  auto frontend = lang::Frontend().analyze("concrete-lambda-packs.gti", R"gti(
int execute<Args...>(Args... args) {
  auto callback = [](int tag, Args... forwarded) -> int { return tag; };
  return callback(3, args...);
}
int main() { return execute() + execute(4) + execute(7, true) - 9; }
)gti");
  expect(
      frontend.canGenerateCode(),
      "concrete lambda packs analyze for empty, single and multiple elements");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  auto built = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, lang::TargetInfo::host());
  expect(built.valid(),
         "detached lambda inventory preserves lexical packs and flattened ABI");
  if (!built.valid()) {
    for (const auto &issue : built.issues)
      std::cerr << issue.detail << '\n';
    return;
  }
  const auto &original = *built.program;
  std::vector<std::size_t> sizes;
  for (const auto &lambda : original.lambdaInstances()) {
    expect(lambda.parameters.size() == 2 && lambda.parameters.back().pack &&
               lambda.parameters.back().type.kind ==
                   lang::SemanticType::TypePack &&
               lambda.parameters.back().type.concretePack,
           "lambda inventory keeps the exact lexical pack binding");
    if (lambda.parameters.size() != 2)
      continue;
    sizes.push_back(lambda.parameters.back().type.arguments.size());
    for (int mutation = 0; mutation != 8; ++mutation) {
      auto changed = original;
      auto &target = lang::LoweredProgramTestAccess::lambdaInstances(
          changed)[lambda.id - 1];
      auto &parameter = target.parameters.back();
      if (mutation == 0)
        parameter.pack = false;
      else if (mutation == 1)
        parameter.type.concretePack = false;
      else if (mutation == 2)
        parameter.type.arguments.push_back(lang::SemanticType::Bool);
      else if (mutation == 3) {
        if (parameter.type.arguments.empty())
          parameter.type.arguments.push_back(lang::SemanticType::Int32);
        else
          parameter.type.arguments.pop_back();
      } else if (mutation == 4)
        parameter.symbol = target.parameters.front().symbol;
      else if (mutation == 5)
        target.parameters.pop_back();
      else if (mutation == 6)
        parameter.access = lang::AccessMode::Mutable;
      else
        target.parameters.front().pack = true;
      expect(hasIssue(lang::verifyLoweredProgram(changed),
                      lang::LoweredProgramIssueKind::InvalidInstanceInventory),
             "malformed lambda pack shape, element, identity and access must "
             "fail detached verification");
    }
  }
  std::sort(sizes.begin(), sizes.end());
  expect(
      sizes == std::vector<std::size_t>{0, 1, 2},
      "all three pack arities survive without invented or dropped parameters");
}

void testConcreteClassAliases() {
  auto frontend = lang::Frontend().analyze("concrete-class-aliases.gti", R"gti(
namespace shapes {
struct Layer<uint64_t N> { Layer<N - 1> inner; int value = 1; };
}
struct shapes::Layer<0> { int value = 100; };
struct Box<uint64_t N> {
  using Previous = shapes::Layer<N - 1>;
  Previous inner;
};
int main() { Box<1> first{}; Box<2> second{}; Box<3> third{}; return 0; }
)gti");
  expect(frontend.canGenerateCode(),
         "concrete class aliases analyze independently");
  if (!frontend.canGenerateCode()) {
    for (const auto &d : frontend.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
    return;
  }
  std::vector<const lang::HirClassInstance *> owners;
  for (const auto &owner : frontend.hir.classInstances())
    if (!owner.typeAliases.empty())
      owners.push_back(&owner);
  expect(owners.size() == 3 && owners[0]->typeAliases[0].declaration != 0 &&
             owners[0]->typeAliases[0].declaration ==
                 owners[1]->typeAliases[0].declaration &&
             owners[0]->typeAliases[0].type != owners[1]->typeAliases[0].type &&
             owners[1]->typeAliases[0].type != owners[2]->typeAliases[0].type,
         "one primary alias identity retains three exact owner-specific types");
  if (owners.size() != 3)
    return;
  bool testedImplicitSource = false;
  for (const auto &constructor : frontend.hir.constructorInstances()) {
    if (!constructor.generatedDefault)
      continue;
    const auto &owner = frontend.hir.classInstances()[constructor.owner - 1];
    for (const auto &field : owner.fields) {
      if (field.declaration == nullptr || field.declaration->initializer())
        continue;
      const auto initialized = std::find_if(
          constructor.initializers.begin(), constructor.initializers.end(),
          [&](const auto &initializer) {
            return initializer.field == field.info.symbol &&
                   initializer.arguments.size() == 1;
          });
      if (initialized == constructor.initializers.end())
        continue;
      auto forged = frontend.hir;
      auto &constructors =
          const_cast<std::vector<lang::HirConstructorInstance> &>(
              forged.constructorInstances());
      auto &body = constructors[constructor.id - 1].body;
      auto value = std::find_if(
          body.values.begin(), body.values.end(), [&](const auto &candidate) {
            return candidate.id == initialized->arguments.front();
          });
      if (value == body.values.end())
        continue;
      value->source = nullptr;
      expect(!lang::verifyHirProgramPlans(frontend.semantics, forged).valid(),
             "implicit nominal default retains exact semantic anchor identity");
      testedImplicitSource = true;
    }
  }
  expect(testedImplicitSource,
         "alias fixture exercises generated implicit nominal construction");
  auto forgedHir = frontend.hir;
  auto &hirClasses = const_cast<std::vector<lang::HirClassInstance> &>(
      forgedHir.classInstances());
  hirClasses[owners[0]->id - 1].typeAliases[0].type =
      owners[1]->typeAliases[0].type;
  expect(!lang::verifyHirProgramPlans(frontend.semantics, forgedHir).valid(),
         "another owner's closed alias type fails HIR verification");
  auto built = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, lang::TargetInfo::host());
  expect(built.valid(), "concrete aliases survive detached lowering");
  if (!built.valid())
    return;
  for (int mutation = 0; mutation < 4; ++mutation) {
    auto forged = *built.program;
    auto &classes = const_cast<std::vector<lang::MirClassInstance> &>(
        lang::LoweredProgramTestAccess::mir(forged).classInstances());
    auto &aliases = classes[owners[0]->id - 1].typeAliases;
    if (mutation == 0)
      aliases.clear();
    if (mutation == 1)
      aliases.push_back(aliases.front());
    if (mutation == 2)
      aliases[0].declaration += 10000;
    if (mutation == 3)
      aliases[0].type = owners[1]->typeAliases[0].type;
    expect(!lang::verifyLoweredProgram(forged).empty(),
           "missing, duplicate, foreign or another owner's alias facts fail "
           "detached verification");
  }
}

void testGeneralRequirementMetadata() {
  auto frontend = lang::Frontend().analyze("lowered-requirements.gti", R"(
constexpr bool accepted<T>() { return true; }
concept selected<T> = accepted<T>();
void named<T>() requires selected<T> {}
void predicate<T>() requires accepted<T>() {}
void expression<T>() requires requires(T& value) {
  { value + value } -> T;
} {}
void nullary<T>() requires requires { { T() } -> T; } {}
void inferred<T>() requires requires(T& value) { { value + value }; } {}
void iteration<T>() requires requires(mut T& range) {
  for (mut int32_t& element : range) {}
} {}
void checked_iteration<T, Result>() requires requires(T& range) {
  for (auto& element : range) {
    static_assert(requires { { element + element } -> Result; });
  }
} {}
struct Cell {
  mut int32_t value = 0;
  mut int32_t& edit() mut { return this.value; }
};
void borrowed<T>() requires requires(mut T& owner) {
  { owner.edit() } -> mut int32_t& [[borrowed_from(owner)]];
} {}
void inferred_borrow<T>() requires requires(mut T& owner) {
  { owner.edit() } -> mut auto& [[borrowed_from(owner)]];
} {}
class Reader {
public:
  int32_t read() { return 7; }
};
void member<T>() requires requires(T& value) {
  { value.read() } -> int32_t;
} {}
class Box {
public:
  Box<T>(T value) requires requires(T& input) { { input + input } -> T; } {}
};
int main() {
  named<int32_t>();
  predicate<int32_t>();
  expression<int32_t>();
  nullary<int32_t>();
  inferred<int32_t>();
  iteration<int32_t[3]>();
  checked_iteration<int32_t[3], int32_t>();
  borrowed<Cell>();
  inferred_borrow<Cell>();
  member<Reader>();
  Box box = Box(1);
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "general requirement metadata fixture is semantically valid");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << "  requirement fixture: " << diagnostic.code << ": "
                << diagnostic.message << '\n';
    return;
  }
  auto built = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, lang::TargetInfo::host());
  expect(built.valid(),
         "concept/member/expression/predicate requirements cross the lowered "
         "boundary without AST pointers or fabricated member names");
  if (!built.valid()) {
    for (const auto &issue : built.issues)
      std::cerr << "  requirement boundary: " << issue.detail << '\n';
    return;
  }
  const auto &program = *built.program;
  const auto *named =
      findPayload<lang::LoweredFunctionDeclaration>(program, "named");
  const auto *member =
      findPayload<lang::LoweredFunctionDeclaration>(program, "member");
  const auto *expression =
      findPayload<lang::LoweredFunctionDeclaration>(program, "expression");
  const auto *nullary =
      findPayload<lang::LoweredFunctionDeclaration>(program, "nullary");
  const auto *predicate =
      findPayload<lang::LoweredFunctionDeclaration>(program, "predicate");
  const auto *borrowed =
      findPayload<lang::LoweredFunctionDeclaration>(program, "borrowed");
  const auto *inferred =
      findPayload<lang::LoweredFunctionDeclaration>(program, "inferred");
  const auto *inferredBorrow =
      findPayload<lang::LoweredFunctionDeclaration>(program, "inferred_borrow");
  expect(inferred && inferred->requirements.size() == 1 &&
             inferred->requirements[0].kind ==
                 lang::LoweredRequirementKind::InferredExpression &&
             !inferred->requirements[0].resultOriginParameter &&
             !inferred->requirements[0].inferredResultAccess &&
             inferredBorrow && inferredBorrow->requirements.size() == 1 &&
             inferredBorrow->requirements[0].inferredResultAccess ==
                 lang::AccessMode::Mutable &&
             inferredBorrow->requirements[0].resultOriginParameter == 0 &&
             inferredBorrow->requirements[0].arguments[1] ==
                 lang::SemanticType::Void,
         "inferred queries retain contracts without publishing unknown "
         "executable types");
  const auto *iteration =
      findPayload<lang::LoweredFunctionDeclaration>(program, "iteration");
  expect(iteration && iteration->requirements.size() == 1 &&
             iteration->requirements[0].kind ==
                 lang::LoweredRequirementKind::Iteration &&
             iteration->requirements[0].mutableReceiver &&
             iteration->requirements[0].resultArgument == 1 &&
             !iteration->requirements[0].resultOriginParameter,
         "loop queries retain element/access facts without inventing an "
         "escaping reference result");
  const auto *checkedIteration = findPayload<lang::LoweredFunctionDeclaration>(
      program, "checked_iteration");
  expect(checkedIteration && checkedIteration->requirements.size() == 1 &&
             checkedIteration->requirements[0].kind ==
                 lang::LoweredRequirementKind::Iteration &&
             checkedIteration->requirements[0].arguments.size() >= 3 &&
             checkedIteration->requirements[0].resultArgument == 1 &&
             !checkedIteration->requirements[0].resultOriginParameter,
         "loop-body result types survive detached requirement metadata");
  expect(named && member && expression && nullary && predicate,
         "all requirement declaration rows are retained");
  if (!named || !member || !expression || !nullary || !predicate)
    return;
  expect(borrowed && borrowed->requirements.size() == 1 &&
             borrowed->requirements[0].resultOriginParameter == 0 &&
             borrowed->requirements[0].arguments[1].kind ==
                 lang::SemanticType::Reference &&
             borrowed->requirements[0].arguments[1].referenceAccess ==
                 lang::AccessMode::Mutable,
         "borrowed requirements retain their exact input origin and access");
  expect(
      named->requirements.size() == 1 &&
          named->requirements[0].kind ==
              lang::LoweredRequirementKind::Concept &&
          named->requirements[0].conceptId != 0 &&
          member->requirements.size() == 1 &&
          member->requirements[0].kind ==
              lang::LoweredRequirementKind::Member &&
          member->requirements[0].memberName == "read",
      "named concept and exact member leaves keep their distinct identities");
  expect(expression->requirements.size() == 1 &&
             expression->requirements[0].kind ==
                 lang::LoweredRequirementKind::Expression &&
             expression->requirements[0].expressionParameterCount == 1 &&
             expression->requirements[0].resultArgument == 1 &&
             expression->requirements[0].arguments.size() == 2 &&
             expression->requirements[0].memberName.empty() &&
             nullary->requirements.size() == 1 &&
             nullary->requirements[0].resultArgument == 0 &&
             nullary->requirements[0].expressionParameterCount == 0 &&
             predicate->requirements.size() == 1 &&
             predicate->requirements[0].kind ==
                 lang::LoweredRequirementKind::Predicate &&
             !predicate->requirements[0].resultArgument &&
             predicate->requirements[0].memberName.empty(),
         "general expressions retain exact result slots and predicates never "
         "masquerade as member requirements");
  const auto text = lang::LoweredProgramPrinter().print(program);
  expect(text.find("kind=expression;concept=0;expression-parameters=1;result-"
                   "argument=1") != std::string::npos &&
             text.find("kind=predicate;concept=0") != std::string::npos,
         "deterministic serialization exposes the explicit requirement kinds");

  std::size_t checkedFunctions = 0;
  std::size_t checkedConstructors = 0;
  for (std::size_t row = 0; row < program.declarations().size(); ++row) {
    const auto &declaration = program.declarations()[row];
    const auto *function =
        std::get_if<lang::LoweredFunctionDeclaration>(&declaration.payload);
    const auto *constructor =
        std::get_if<lang::LoweredConstructorDeclaration>(&declaration.payload);
    const auto *requirements = function      ? &function->requirements
                               : constructor ? &constructor->requirements
                                             : nullptr;
    if (!requirements || requirements->empty())
      continue;
    if (function)
      ++checkedFunctions;
    else
      ++checkedConstructors;
    for (int mutation = 0; mutation < 8; ++mutation) {
      auto forged = program;
      auto &payload =
          lang::LoweredProgramTestAccess::declarations(forged)[row].payload;
      auto &requirement =
          function ? std::get<lang::LoweredFunctionDeclaration>(payload)
                         .requirements[0]
                   : std::get<lang::LoweredConstructorDeclaration>(payload)
                         .requirements[0];
      if (mutation == 0)
        requirement.kind = lang::LoweredRequirementKind::Count;
      else if (mutation == 1)
        requirement.resultArgument = requirement.arguments.size();
      else if (mutation == 2)
        requirement.expressionParameterCount = requirement.arguments.size() + 1;
      else if (mutation == 3)
        requirement.arguments.push_back(lang::SemanticType::Unknown);
      else if (mutation == 4)
        requirement.conceptId = requirement.conceptId == 0 ? 999999 : 0;
      else if (mutation == 5) {
        requirement.kind = lang::LoweredRequirementKind::Expression;
        requirement.resultArgument.reset();
      } else if (mutation == 6) {
        requirement.kind = lang::LoweredRequirementKind::Predicate;
        requirement.requiresUnsafe = true;
      } else {
        requirement.resultOriginParameter = requirement.arguments.size();
      }
      expect(
          hasIssue(lang::verifyLoweredProgram(forged),
                   lang::LoweredProgramIssueKind::InvalidDeclarationInventory),
          "invalid requirement kinds, mixed payloads, result slots and "
          "unresolved types are rejected before backend consumption");
    }
    if ((*requirements)[0].kind == lang::LoweredRequirementKind::Expression ||
        (*requirements)[0].kind ==
            lang::LoweredRequirementKind::InferredExpression ||
        (*requirements)[0].kind == lang::LoweredRequirementKind::Iteration ||
        (*requirements)[0].kind == lang::LoweredRequirementKind::Predicate) {
      auto forged = program;
      auto &payload =
          lang::LoweredProgramTestAccess::declarations(forged)[row].payload;
      auto &requirement =
          function ? std::get<lang::LoweredFunctionDeclaration>(payload)
                         .requirements[0]
                   : std::get<lang::LoweredConstructorDeclaration>(payload)
                         .requirements[0];
      requirement.source = {};
      expect(
          hasIssue(lang::verifyLoweredProgram(forged),
                   lang::LoweredProgramIssueKind::InvalidDeclarationInventory),
          "general requirement leaves must retain source provenance");
    }
    if ((*requirements)[0].kind == lang::LoweredRequirementKind::Expression) {
      if ((*requirements)[0].resultOriginParameter) {
        auto forged = program;
        auto &payload =
            lang::LoweredProgramTestAccess::declarations(forged)[row].payload;
        auto &requirement =
            function ? std::get<lang::LoweredFunctionDeclaration>(payload)
                           .requirements[0]
                     : std::get<lang::LoweredConstructorDeclaration>(payload)
                           .requirements[0];
        requirement.arguments[*requirement.resultOriginParameter]
            .referenceAccess = lang::AccessMode::ReadOnly;
        expect(
            hasIssue(
                lang::verifyLoweredProgram(forged),
                lang::LoweredProgramIssueKind::InvalidDeclarationInventory),
            "a mutable result cannot acquire a read-only origin in lowering");
      }
      auto forged = program;
      auto &payload =
          lang::LoweredProgramTestAccess::declarations(forged)[row].payload;
      auto &requirement =
          function ? std::get<lang::LoweredFunctionDeclaration>(payload)
                         .requirements[0]
                   : std::get<lang::LoweredConstructorDeclaration>(payload)
                         .requirements[0];
      auto &result = requirement.arguments[*requirement.resultArgument];
      if (requirement.resultOriginParameter)
        requirement.resultOriginParameter.reset();
      else
        result = lang::SemanticType::referenceTo(result);
      expect(
          hasIssue(lang::verifyLoweredProgram(forged),
                   lang::LoweredProgramIssueKind::InvalidDeclarationInventory),
          "general expression result slots must not silently become borrows");
    }
  }
  expect(checkedFunctions >= 5 && checkedConstructors >= 1,
         "requirement mutation coverage includes functions and constructors");
}

void testClassDefinitionFamilyIdentity() {
  const auto frontend = lang::Frontend().analyze("definition-family.gti", R"(
class Record<T, E> { public: int kind() { return 1; } };
class Record<void, E> { public: int kind() { return 2; } };
class Unrelated<T, E> {};
int main() {
  Record<int, int> value{};
  Record<void, int> empty{};
  return value.kind() + empty.kind() - 3;
}
)");
  expect(frontend.canGenerateCode(), "definition family fixture is valid");
  if (!frontend.canGenerateCode())
    return;
  auto built = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, lang::TargetInfo::host());
  expect(built.valid(), "definition family crosses the lowered boundary");
  if (!built.valid())
    return;
  std::size_t memberIndex = built.program->declarations().size();
  lang::ClassId unrelated = 0;
  for (std::size_t index = 0; index < built.program->declarations().size();
       ++index) {
    const auto &declaration = built.program->declarations()[index];
    const auto *type =
        std::get_if<lang::LoweredClassDeclaration>(&declaration.payload);
    if (!type)
      continue;
    if (type->definitionFamilyPrimary != 0)
      memberIndex = index;
    if (declaration.name == "Unrelated")
      unrelated = type->id;
  }
  expect(memberIndex < built.program->declarations().size() && unrelated != 0,
         "family fixture retains its member and unrelated identity");
  if (memberIndex >= built.program->declarations().size() || unrelated == 0)
    return;
  for (int mutation = 0; mutation < 3; ++mutation) {
    auto forged = *built.program;
    auto &member = std::get<lang::LoweredClassDeclaration>(
        lang::LoweredProgramTestAccess::declarations(forged)[memberIndex]
            .payload);
    member.definitionFamilyPrimary = mutation == 0   ? member.id
                                     : mutation == 1 ? unrelated
                                                     : 999999;
    expect(hasIssue(lang::verifyLoweredProgram(forged),
                    lang::LoweredProgramIssueKind::InvalidDeclarationInventory),
           "self, unrelated and missing class family identities are rejected");
  }
}

} // namespace

int main() {
  testSourceDebugRecordOwnership();
  testSemanticSourceDebugProducers();
  testInitializerSourceDebugProducers();
  testFrontendSourceDebugProfile();
  testMirSourceDebugTransport();
  testDebugPlaceAvailability();
  testDebugLifetimeAndBorrowAvailability();
  testDebugConcreteEntryAvailability();
  testDebugPackFragments();
  testDetachedDeterministicProgram();
  testBuilderRejectsForgedOptimizationEvidence();
  testDetachedDebugLocations();
  testDetachedDebugRanges();
  testDebugValueEvents();
  testDebugReceiverFragments();
  testVerificationEvidenceOwnsItsProgram();
  testMutationRejection();
  testConcreteBranchSymbols();
  testConcreteLambdaPacks();
  testConcreteClassAliases();
  testGeneralRequirementMetadata();
  testClassDefinitionFamilyIdentity();
  if (failures != 0) {
    std::cerr << failures << " lowered-program test(s) failed\n";
    return 1;
  }
  std::cout << "lowered-program contract tests passed\n";
  return 0;
}
