#pragma once

#include "gti/driver/build.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lang::driver {

// The build-report sidecar is a driver contract rather than CLI presentation:
// `gti verify` reads a recorded report back and compares it against a fresh
// capture, so writing and reading the format live together and round-trip
// exactly.
[[nodiscard]] std::string
serializeBuildProvenanceJson(const BuildProvenance &provenance);

// Parses a sidecar written by serializeBuildProvenanceJson. Every field the
// record needs is required; a missing or wrongly typed member reports the
// JSON path that failed rather than silently defaulting, because a defaulted
// field would compare equal by accident. Only the current schema is
// interpreted.
[[nodiscard]] std::optional<BuildProvenance>
parseBuildProvenanceJson(std::string_view text, std::string &errorMessage);

// Two native argv operands are owned by the driver and unique to one
// invocation: the staged link output that follows `-o`, and the build-report
// dependency file that follows `-MF`. Both carry a process/nonce identity, so
// comparing them literally would report a difference for every verification.
// They are replaced by stable placeholders; every other operand, absolute
// paths included, compares exactly.
[[nodiscard]] std::vector<std::string>
normalizeNativeCommandForComparison(const std::vector<std::string> &command);

// One named divergence between a recorded and a freshly captured record.
// `subject` names the thing that differs (a source unit's logical identity, a
// runtime header, the native compiler), `field` the compared fact.
struct BuildProvenanceDifference {
  std::string subject;
  std::string field;
  std::string expected;
  std::string actual;
};

// Compares every content identity, identity aggregate, project selection
// fact, normalized native argv operand, and the produced artifact. Source
// units and runtime headers are matched by logical identity, so a relocated
// tree with identical content compares equal while a changed unit is named
// exactly. Canonical paths are context, not identity: a file whose content
// matches at a different path is not a difference.
[[nodiscard]] std::vector<BuildProvenanceDifference>
compareBuildProvenance(const BuildProvenance &recorded,
                       const BuildProvenance &observed);

} // namespace lang::driver
