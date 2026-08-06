#pragma once

#include "gti/optimizer.h"

#include <string>
#include <string_view>

namespace lang {

enum class BackendArtifactKind {
  Source,
  Object,
};

struct BackendInput {
  const Program &program;
  const SemanticModel &semantics;
  const OptimizationResult &optimizations;
  TargetInfo target = TargetInfo::host();
};

struct BackendArtifact {
  BackendArtifactKind kind = BackendArtifactKind::Source;
  std::string contents;
  std::string extension;
};

class Backend {
public:
  Backend() = default;
  Backend(const Backend &) = delete;
  Backend &operator=(const Backend &) = delete;
  virtual ~Backend() = default;

  [[nodiscard]] virtual std::string_view name() const = 0;
  [[nodiscard]] virtual BackendArtifact generate(const BackendInput &input) = 0;
};

} // namespace lang
