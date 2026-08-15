#pragma once

#include "gti/failure.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lang {

class HirProgram;
class SourceManager;
class SourceGraph;

using FailureSiteId = std::uint32_t;

struct FailureArtifactIdentity {
  std::array<std::uint8_t, 32> bytes{};

  [[nodiscard]] bool isZero() const;
  [[nodiscard]] std::string hex() const;

  friend bool operator==(const FailureArtifactIdentity &,
                         const FailureArtifactIdentity &) = default;
};

struct FailureSourceDescriptor {
  SourceUnitId sourceUnit = 0;
  std::string logicalName;

  friend bool operator==(const FailureSourceDescriptor &,
                         const FailureSourceDescriptor &) = default;
};

struct FailureSiteDescriptor {
  FailureSiteId id = 0;
  std::string logicalSource;
  int line = 1;
  std::size_t start = 0;
  std::size_t end = 0;
  std::vector<DefinedFailureOutcome> outcomes;

  friend bool operator==(const FailureSiteDescriptor &,
                         const FailureSiteDescriptor &) = default;
};

struct FailureOriginAssignment {
  SourceUnitId sourceUnit = 0;
  std::size_t start = 0;
  std::size_t end = 0;
  int line = 1;
  std::vector<DefinedFailureOutcome> outcomes;
  FailureSiteId site = 0;

  friend bool operator==(const FailureOriginAssignment &,
                         const FailureOriginAssignment &) = default;
};

class FailureMetadata {
public:
  [[nodiscard]] const FailureArtifactIdentity &artifactIdentity() const {
    return identity;
  }

  [[nodiscard]] const std::vector<std::uint8_t> &descriptorBytes() const {
    return serialization;
  }

  [[nodiscard]] const std::vector<FailureSourceDescriptor> &
  sourceUnits() const {
    return sources;
  }

  [[nodiscard]] const std::vector<FailureSiteDescriptor> &sites() const {
    return siteTable;
  }

  [[nodiscard]] const std::vector<FailureOriginAssignment> &
  assignments() const {
    return originAssignments;
  }

  [[nodiscard]] const FailureSiteDescriptor *findSite(FailureSiteId id) const;

  [[nodiscard]] std::optional<FailureSiteId>
  siteFor(const DefinedFailureOrigin &origin) const;

  friend bool operator==(const FailureMetadata &,
                         const FailureMetadata &) = default;

private:
  friend class FailureMetadataBuilder;

  FailureArtifactIdentity identity;
  std::vector<std::uint8_t> serialization;
  std::vector<FailureSourceDescriptor> sources;
  std::vector<FailureSiteDescriptor> siteTable;
  std::vector<FailureOriginAssignment> originAssignments;
};

struct FailureMetadataVerificationResult {
  std::vector<std::string> errors;

  [[nodiscard]] bool valid() const { return errors.empty(); }
};

[[nodiscard]] FailureMetadataVerificationResult
verifyFailureMetadata(const FailureMetadata &metadata);

struct FailureMetadataBuildResult {
  FailureMetadata metadata;
  std::vector<std::string> errors;

  [[nodiscard]] bool valid() const { return errors.empty(); }
};

class FailureMetadataBuilder {
public:
  [[nodiscard]] FailureMetadataBuildResult
  build(const SourceGraph &sourceGraph, const SourceManager &sources,
        const HirProgram &hir, const std::filesystem::path &entryPath) const;
};

} // namespace lang
