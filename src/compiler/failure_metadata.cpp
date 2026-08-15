#include "gti/failure_metadata.h"

#include "gti/diagnostic.h"
#include "gti/hir.h"
#include "gti/source_graph.h"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/SHA256.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lang {
namespace {

constexpr std::string_view artifactPrefix = "GTI-FAILURE-ARTIFACT-V1";
constexpr std::string_view externalSourcePrefix = "GTI-EXTERNAL-SOURCE-V1";

struct OriginKey {
  SourceUnitId sourceUnit = 0;
  std::size_t start = 0;
  std::size_t end = 0;

  friend bool operator<(const OriginKey &left, const OriginKey &right) {
    return std::tie(left.sourceUnit, left.start, left.end) <
           std::tie(right.sourceUnit, right.start, right.end);
  }
};

struct PendingOrigin {
  OriginKey key;
  int line = 1;
  std::vector<DefinedFailureOutcome> outcomes;
};

struct RouteEdge {
  std::string spelling;
  std::size_t occurrence = 0;
};

struct SourceRoute {
  std::vector<RouteEdge> edges;
  std::vector<std::uint8_t> orderingBytes;
};

[[nodiscard]] bool byteLess(std::string_view left, std::string_view right) {
  return std::lexicographical_compare(left.begin(), left.end(), right.begin(),
                                      right.end(), [](char lhs, char rhs) {
                                        return static_cast<unsigned char>(lhs) <
                                               static_cast<unsigned char>(rhs);
                                      });
}

[[nodiscard]] bool outcomeLess(DefinedFailureOutcome left,
                               DefinedFailureOutcome right) {
  if (left.code != right.code) {
    return left.code < right.code;
  }
  return byteLess(definedFailureDetailName(left.detail),
                  definedFailureDetailName(right.detail));
}

void sortAndUniqueOutcomes(std::vector<DefinedFailureOutcome> &outcomes) {
  std::sort(outcomes.begin(), outcomes.end(), outcomeLess);
  outcomes.erase(std::unique(outcomes.begin(), outcomes.end()), outcomes.end());
}

void appendUInt64(std::vector<std::uint8_t> &output, std::uint64_t value) {
  for (unsigned index = 0; index < 8; ++index) {
    output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

void appendBytes(std::vector<std::uint8_t> &output, std::string_view value) {
  appendUInt64(output, value.size());
  output.insert(output.end(), value.begin(), value.end());
}

void appendPrefix(std::vector<std::uint8_t> &output, std::string_view prefix) {
  output.insert(output.end(), prefix.begin(), prefix.end());
  output.push_back(0);
}

[[nodiscard]] FailureArtifactIdentity
hashBytes(const std::vector<std::uint8_t> &bytes) {
  FailureArtifactIdentity result;
  result.bytes = llvm::SHA256::hash(
      llvm::ArrayRef<std::uint8_t>(bytes.data(), bytes.size()));
  return result;
}

[[nodiscard]] std::vector<std::uint8_t>
serializeSites(const std::vector<FailureSiteDescriptor> &sites) {
  std::vector<std::uint8_t> result;
  appendPrefix(result, artifactPrefix);
  for (const FailureSiteDescriptor &site : sites) {
    appendBytes(result, site.logicalSource);
    appendUInt64(result, static_cast<std::uint64_t>(site.line));
    appendUInt64(result, site.start);
    appendUInt64(result, site.end);
    appendUInt64(result, site.outcomes.size());
    for (const DefinedFailureOutcome outcome : site.outcomes) {
      appendUInt64(result, static_cast<std::uint16_t>(outcome.code));
      appendBytes(result, definedFailureDetailName(outcome.detail));
    }
  }
  return result;
}

[[nodiscard]] bool sameSiteRecord(const FailureSiteDescriptor &left,
                                  const FailureSiteDescriptor &right) {
  return left.logicalSource == right.logicalSource && left.line == right.line &&
         left.start == right.start && left.end == right.end &&
         left.outcomes == right.outcomes;
}

[[nodiscard]] bool siteLess(const FailureSiteDescriptor &left,
                            const FailureSiteDescriptor &right) {
  if (left.logicalSource != right.logicalSource) {
    return byteLess(left.logicalSource, right.logicalSource);
  }
  if (left.start != right.start) {
    return left.start < right.start;
  }
  if (left.end != right.end) {
    return left.end < right.end;
  }
  return std::lexicographical_compare(
      left.outcomes.begin(), left.outcomes.end(), right.outcomes.begin(),
      right.outcomes.end(), outcomeLess);
}

[[nodiscard]] bool assignmentLess(const FailureOriginAssignment &left,
                                  const FailureOriginAssignment &right) {
  return std::tie(left.sourceUnit, left.start, left.end) <
         std::tie(right.sourceUnit, right.start, right.end);
}

[[nodiscard]] bool containedRelativePath(const std::filesystem::path &root,
                                         const std::filesystem::path &path,
                                         std::string &relative) {
  const std::filesystem::path candidate = path.lexically_relative(root);
  if (candidate.empty() || candidate.is_absolute()) {
    return false;
  }
  for (const std::filesystem::path &component : candidate) {
    if (component == "..") {
      return false;
    }
  }
  relative = candidate.generic_string();
  return !relative.empty() && relative != ".";
}

[[nodiscard]] std::optional<std::string>
canonicalLogicalRelativePath(std::string_view value) {
  const std::filesystem::path path(value);
  if (path.empty() || path.is_absolute() || path.has_root_name() ||
      path.has_root_directory()) {
    return std::nullopt;
  }
  const std::filesystem::path normalized = path.lexically_normal();
  for (const std::filesystem::path &component : normalized) {
    if (component == "..") {
      return std::nullopt;
    }
  }
  const std::string result = normalized.generic_string();
  if (result.empty() || result == ".") {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] std::vector<std::uint8_t>
encodeRoute(const std::vector<RouteEdge> &edges) {
  std::vector<std::uint8_t> result;
  appendUInt64(result, edges.size());
  for (const RouteEdge &edge : edges) {
    appendBytes(result, edge.spelling);
    appendUInt64(result, edge.occurrence);
  }
  return result;
}

[[nodiscard]] std::unordered_map<SourceUnitId, SourceRoute>
selectRoutes(const SourceGraph &graph) {
  struct Candidate {
    SourceUnitId unit = 0;
    SourceRoute route;
  };
  const auto worse = [](const Candidate &left, const Candidate &right) {
    if (left.route.edges.size() != right.route.edges.size()) {
      return left.route.edges.size() > right.route.edges.size();
    }
    return left.route.orderingBytes > right.route.orderingBytes;
  };
  std::priority_queue<Candidate, std::vector<Candidate>, decltype(worse)> queue(
      worse);
  SourceRoute root;
  root.orderingBytes = encodeRoute(root.edges);
  queue.push({.unit = graph.entryUnit(), .route = root});

  std::unordered_map<SourceUnitId, SourceRoute> selected;
  while (!queue.empty()) {
    Candidate candidate = queue.top();
    queue.pop();
    const auto existing = selected.find(candidate.unit);
    if (existing != selected.end()) {
      continue;
    }
    selected.emplace(candidate.unit, candidate.route);
    for (const SourceDependency &dependency : graph.dependencyEdges()) {
      if (dependency.source != candidate.unit) {
        continue;
      }
      SourceRoute next = candidate.route;
      if (!dependency.includeSpelling.empty()) {
        next.edges.push_back({.spelling = dependency.includeSpelling,
                              .occurrence = dependency.includeOccurrence});
      } else if (dependency.kind != SourceDependencyKind::Prelude) {
        continue;
      }
      next.orderingBytes = encodeRoute(next.edges);
      queue.push({.unit = dependency.target, .route = std::move(next)});
    }
  }
  return selected;
}

[[nodiscard]] std::optional<std::string>
externalLogicalName(const SourceUnit &unit, const SourceManager &sources,
                    const std::unordered_map<SourceUnitId, SourceRoute> &routes,
                    std::vector<std::string> &errors) {
  const std::string *source = sources.find(unit.path.string());
  const auto route = routes.find(unit.id);
  if (source == nullptr || route == routes.end()) {
    errors.push_back("failure metadata cannot resolve source bytes or an "
                     "include route for source unit " +
                     std::to_string(unit.id));
    return std::nullopt;
  }
  std::vector<std::uint8_t> identityBytes;
  appendPrefix(identityBytes, externalSourcePrefix);
  appendBytes(identityBytes, *source);
  const std::vector<std::uint8_t> routeBytes = encodeRoute(route->second.edges);
  identityBytes.insert(identityBytes.end(), routeBytes.begin(),
                       routeBytes.end());
  const FailureArtifactIdentity identity = hashBytes(identityBytes);
  const std::string basename = unit.path.filename().generic_string();
  if (identity.isZero() || basename.empty()) {
    errors.push_back("failure metadata produced an invalid external source "
                     "identity for source unit " +
                     std::to_string(unit.id));
    return std::nullopt;
  }
  return "<external>/" + identity.hex() + "/" + basename;
}

[[nodiscard]] std::optional<std::string>
logicalName(const SourceUnit &unit, const SourceUnit *entry,
            const std::filesystem::path &directRoot,
            const SourceManager &sources,
            const std::unordered_map<SourceUnitId, SourceRoute> &routes,
            std::vector<std::string> &errors) {
  if (unit.prelude || unit.role == SourceUnitRole::Prelude) {
    return "<prelude>";
  }
  if (unit.standardLibraryName) {
    return "<" + *unit.standardLibraryName + ">";
  }
  if (entry != nullptr && entry->packageIdentity && unit.packageIdentity &&
      *entry->packageIdentity == *unit.packageIdentity &&
      unit.packageRelativePath && !unit.packageRelativePath->empty()) {
    if (std::optional<std::string> relative =
            canonicalLogicalRelativePath(*unit.packageRelativePath)) {
      return relative;
    }
    errors.push_back("failure metadata received an invalid package-relative "
                     "path for source unit " +
                     std::to_string(unit.id));
    return std::nullopt;
  }
  if (entry == nullptr || !entry->packageIdentity) {
    std::string relative;
    if (containedRelativePath(directRoot, unit.path, relative)) {
      return relative;
    }
  }
  return externalLogicalName(unit, sources, routes, errors);
}

template <typename Callback>
void forEachHirBody(const HirProgram &hir, Callback callback) {
  callback(hir.module());
  for (const HirClassInstance &instance : hir.classInstances()) {
    callback(instance.fieldInitializers);
    callback(instance.staticFieldInitializers);
  }
  for (const HirFunctionInstance &instance : hir.functionInstances()) {
    callback(instance.body);
  }
  for (const HirConstructorInstance &instance : hir.constructorInstances()) {
    callback(instance.body);
  }
  for (const HirDestructorInstance &instance : hir.destructorInstances()) {
    callback(instance.body);
  }
  for (const HirLambda &lambda : hir.lambdaInstances()) {
    callback(lambda.body);
  }
}

[[nodiscard]] bool exactHostedFailure(const DefinedFailureOperation &operation,
                                      DefinedFailureCode code,
                                      DefinedFailureDetail detail,
                                      const HirHostedProgramEntryPlan &plan) {
  if (operation.propagation != FailurePropagationKind::None ||
      operation.localOrigins.size() != 1) {
    return false;
  }
  const DefinedFailureOrigin &origin = operation.localOrigins.front();
  return origin.outcomes ==
             std::vector<DefinedFailureOutcome>{
                 {.code = code, .detail = detail}} &&
         origin.sourceUnit == plan.sourceUnit &&
         origin.start == plan.mainAnchor.start &&
         origin.end == plan.mainAnchor.end &&
         origin.line == plan.mainAnchor.line;
}

[[nodiscard]] bool
outcomesValidAndSorted(const std::vector<DefinedFailureOutcome> &outcomes) {
  if (outcomes.empty()) {
    return false;
  }
  for (std::size_t index = 0; index < outcomes.size(); ++index) {
    if (!validDefinedFailureOutcome(outcomes[index]) ||
        (index != 0 && !outcomeLess(outcomes[index - 1], outcomes[index]))) {
      return false;
    }
  }
  return true;
}

} // namespace

bool FailureArtifactIdentity::isZero() const {
  return std::all_of(bytes.begin(), bytes.end(),
                     [](std::uint8_t byte) { return byte == 0; });
}

std::string FailureArtifactIdentity::hex() const {
  static constexpr std::array<char, 16> digits = {'0', '1', '2', '3', '4', '5',
                                                  '6', '7', '8', '9', 'a', 'b',
                                                  'c', 'd', 'e', 'f'};
  std::string result;
  result.reserve(bytes.size() * 2);
  for (const std::uint8_t byte : bytes) {
    result.push_back(digits[byte >> 4U]);
    result.push_back(digits[byte & 0x0FU]);
  }
  return result;
}

const FailureSiteDescriptor *FailureMetadata::findSite(FailureSiteId id) const {
  return id == 0 || id > siteTable.size() ? nullptr : &siteTable[id - 1];
}

std::optional<FailureSiteId>
FailureMetadata::siteFor(const DefinedFailureOrigin &origin) const {
  const FailureOriginAssignment key{.sourceUnit = origin.sourceUnit,
                                    .start = origin.start,
                                    .end = origin.end};
  const auto found = std::lower_bound(
      originAssignments.begin(), originAssignments.end(), key, assignmentLess);
  if (found == originAssignments.end() ||
      found->sourceUnit != origin.sourceUnit || found->start != origin.start ||
      found->end != origin.end ||
      std::any_of(origin.outcomes.begin(), origin.outcomes.end(),
                  [&](DefinedFailureOutcome outcome) {
                    return std::find(found->outcomes.begin(),
                                     found->outcomes.end(),
                                     outcome) == found->outcomes.end();
                  })) {
    return std::nullopt;
  }
  return found->site;
}

FailureMetadataVerificationResult
verifyFailureMetadata(const FailureMetadata &metadata) {
  FailureMetadataVerificationResult result;
  const auto report = [&](std::string message) {
    result.errors.push_back(std::move(message));
  };

  for (std::size_t index = 0; index < metadata.sourceUnits().size(); ++index) {
    const FailureSourceDescriptor &source = metadata.sourceUnits()[index];
    if (source.sourceUnit == 0 || source.logicalName.empty() ||
        (index != 0 &&
         metadata.sourceUnits()[index - 1].sourceUnit >= source.sourceUnit)) {
      report("failure source descriptors are invalid or not strictly sorted");
      break;
    }
  }

  for (std::size_t index = 0; index < metadata.sites().size(); ++index) {
    const FailureSiteDescriptor &site = metadata.sites()[index];
    if (site.id != index + 1 || site.logicalSource.empty() || site.line < 1 ||
        site.end <= site.start || !outcomesValidAndSorted(site.outcomes) ||
        (index != 0 && !siteLess(metadata.sites()[index - 1], site))) {
      report("failure site table is invalid or not canonically sorted");
      break;
    }
  }

  std::vector<bool> referenced(metadata.sites().size(), false);
  for (std::size_t index = 0; index < metadata.assignments().size(); ++index) {
    const FailureOriginAssignment &assignment = metadata.assignments()[index];
    const FailureSiteDescriptor *site = metadata.findSite(assignment.site);
    const auto source = std::lower_bound(
        metadata.sourceUnits().begin(), metadata.sourceUnits().end(),
        assignment.sourceUnit,
        [](const FailureSourceDescriptor &candidate, SourceUnitId sourceUnit) {
          return candidate.sourceUnit < sourceUnit;
        });
    if (assignment.sourceUnit == 0 || assignment.line < 1 ||
        assignment.end <= assignment.start ||
        !outcomesValidAndSorted(assignment.outcomes) || site == nullptr ||
        source == metadata.sourceUnits().end() ||
        source->sourceUnit != assignment.sourceUnit ||
        site->logicalSource != source->logicalName ||
        site->line != assignment.line || site->start != assignment.start ||
        site->end != assignment.end || site->outcomes != assignment.outcomes ||
        (index != 0 &&
         !assignmentLess(metadata.assignments()[index - 1], assignment))) {
      report("failure origin assignments do not exactly match the site table");
      break;
    }
    referenced[assignment.site - 1] = true;
  }
  if (std::any_of(referenced.begin(), referenced.end(),
                  [](bool value) { return !value; })) {
    report("failure site table contains an unreferenced site");
  }

  const std::vector<std::uint8_t> expected = serializeSites(metadata.sites());
  if (metadata.descriptorBytes() != expected) {
    report("failure descriptor serialization does not match its site table");
  }
  const FailureArtifactIdentity expectedIdentity = hashBytes(expected);
  if (metadata.artifactIdentity().isZero() ||
      metadata.artifactIdentity() != expectedIdentity) {
    report("failure artifact identity does not match its descriptor bytes");
  }
  return result;
}

FailureMetadataBuildResult FailureMetadataBuilder::build(
    const SourceGraph &sourceGraph, const SourceManager &sources,
    const HirProgram &hir, const std::filesystem::path &entryPath) const {
  FailureMetadataBuildResult result;
  const SourceUnit *entry = sourceGraph.findUnit(sourceGraph.entryUnit());
  const std::filesystem::path directRoot =
      entry == nullptr ? entryPath.parent_path() : entry->path.parent_path();
  const std::unordered_map<SourceUnitId, SourceRoute> routes =
      selectRoutes(sourceGraph);

  std::map<OriginKey, PendingOrigin> origins;
  const auto collectOrigin = [&](const DefinedFailureOrigin &origin) {
    const SourceUnit *unit = sourceGraph.findUnit(origin.sourceUnit);
    const std::string *source =
        unit == nullptr ? nullptr : sources.find(unit->path.string());
    if (unit == nullptr || source == nullptr || origin.end <= origin.start ||
        origin.end > source->size() || origin.outcomes.empty()) {
      result.errors.push_back(
          "failure origin cannot be resolved to exact source bytes");
      return;
    }
    const std::size_t precedingLines = static_cast<std::size_t>(
        std::count(source->begin(), source->begin() + origin.start, '\n'));
    if (precedingLines >=
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      result.errors.push_back(
          "failure origin line exceeds the metadata line-number range");
      return;
    }
    const int exactLine = 1 + static_cast<int>(precedingLines);
    if (origin.line != exactLine) {
      result.errors.push_back(
          "failure origin line does not match its exact source bytes");
      return;
    }
    PendingOrigin &pending =
        origins[{origin.sourceUnit, origin.start, origin.end}];
    pending.key = {origin.sourceUnit, origin.start, origin.end};
    pending.line = exactLine;
    pending.outcomes.insert(pending.outcomes.end(), origin.outcomes.begin(),
                            origin.outcomes.end());
  };
  forEachHirBody(hir, [&](const HirBody &body) {
    for (const HirValue &value : body.values) {
      for (const DefinedFailureOrigin &origin :
           value.definedFailure.localOrigins) {
        collectOrigin(origin);
      }
    }
  });
  if (const std::optional<HirHostedProgramEntryPlan> &hosted =
          hir.hostedProgramEntryPlan()) {
    if (hosted->kind == ProgramEntryKind::NoArguments) {
      if (!hosted->validateCount.empty() || !hosted->convertCount.empty()) {
        result.errors.push_back(
            "no-argument hosted entry contains failure origins");
      }
    } else if (hosted->kind != ProgramEntryKind::OwnedArguments ||
               !exactHostedFailure(
                   hosted->validateCount,
                   DefinedFailureCode::HostedRuntimeContractFailure,
                   DefinedFailureDetail::NegativeArgumentCount, *hosted) ||
               !exactHostedFailure(
                   hosted->convertCount,
                   DefinedFailureCode::NumericConversionOutOfRange,
                   DefinedFailureDetail::HostedArgumentCount, *hosted)) {
      result.errors.push_back(
          "owned-argument hosted entry has invalid failure origins");
    } else {
      collectOrigin(hosted->validateCount.localOrigins.front());
      collectOrigin(hosted->convertCount.localOrigins.front());
    }
  }
  for (auto &[_, origin] : origins) {
    sortAndUniqueOutcomes(origin.outcomes);
    if (!outcomesValidAndSorted(origin.outcomes)) {
      result.errors.push_back("failure origin contains invalid outcomes");
    }
  }
  if (!result.errors.empty()) {
    return result;
  }

  std::unordered_map<SourceUnitId, std::string> logicalNames;
  for (const auto &[key, _] : origins) {
    if (logicalNames.contains(key.sourceUnit)) {
      continue;
    }
    const SourceUnit *unit = sourceGraph.findUnit(key.sourceUnit);
    if (unit == nullptr) {
      result.errors.push_back(
          "failure origin references an unknown source unit");
      continue;
    }
    std::optional<std::string> name =
        logicalName(*unit, entry, directRoot, sources, routes, result.errors);
    if (name) {
      logicalNames.emplace(key.sourceUnit, std::move(*name));
    }
  }
  if (!result.errors.empty()) {
    return result;
  }

  result.metadata.sources.reserve(logicalNames.size());
  for (const auto &[sourceUnit, name] : logicalNames) {
    result.metadata.sources.push_back(
        {.sourceUnit = sourceUnit, .logicalName = name});
  }
  std::sort(result.metadata.sources.begin(), result.metadata.sources.end(),
            [](const FailureSourceDescriptor &left,
               const FailureSourceDescriptor &right) {
              return left.sourceUnit < right.sourceUnit;
            });

  struct PendingSite {
    FailureSiteDescriptor descriptor;
    PendingOrigin origin;
  };
  std::vector<PendingSite> pendingSites;
  pendingSites.reserve(origins.size());
  for (const auto &[key, origin] : origins) {
    pendingSites.push_back(
        {.descriptor = {.logicalSource = logicalNames.at(key.sourceUnit),
                        .line = origin.line,
                        .start = key.start,
                        .end = key.end,
                        .outcomes = origin.outcomes},
         .origin = origin});
  }
  std::sort(pendingSites.begin(), pendingSites.end(),
            [](const PendingSite &left, const PendingSite &right) {
              return siteLess(left.descriptor, right.descriptor);
            });

  for (const PendingSite &pending : pendingSites) {
    FailureSiteId site = 0;
    if (!result.metadata.siteTable.empty() &&
        sameSiteRecord(result.metadata.siteTable.back(), pending.descriptor)) {
      site = result.metadata.siteTable.back().id;
    } else {
      if (result.metadata.siteTable.size() >=
          std::numeric_limits<FailureSiteId>::max()) {
        result.errors.push_back(
            "failure site table exceeds its fixed ID range");
        return result;
      }
      FailureSiteDescriptor descriptor = pending.descriptor;
      descriptor.id =
          static_cast<FailureSiteId>(result.metadata.siteTable.size() + 1);
      site = descriptor.id;
      result.metadata.siteTable.push_back(std::move(descriptor));
    }
    result.metadata.originAssignments.push_back(
        {.sourceUnit = pending.origin.key.sourceUnit,
         .start = pending.origin.key.start,
         .end = pending.origin.key.end,
         .line = pending.origin.line,
         .outcomes = pending.origin.outcomes,
         .site = site});
  }
  std::sort(result.metadata.originAssignments.begin(),
            result.metadata.originAssignments.end(), assignmentLess);
  result.metadata.serialization = serializeSites(result.metadata.siteTable);
  result.metadata.identity = hashBytes(result.metadata.serialization);
  if (result.metadata.identity.isZero()) {
    result.errors.push_back("failure artifact identity uses the reserved zero "
                            "value");
    return result;
  }
  const FailureMetadataVerificationResult verification =
      verifyFailureMetadata(result.metadata);
  result.errors.insert(result.errors.end(), verification.errors.begin(),
                       verification.errors.end());
  return result;
}

} // namespace lang
