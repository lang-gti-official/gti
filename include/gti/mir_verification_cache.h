#pragma once

#include <memory>

namespace lang {
namespace mir_detail {
class VerificationSession;
}

// Bounded storage for structural queries shared by editor snapshots. Entries
// own only integer graph identities and facts. They cannot certify a program.
class MirVerificationCache final {
public:
  MirVerificationCache();
  ~MirVerificationCache();

private:
  friend class mir_detail::VerificationSession;
  struct Storage;
  std::unique_ptr<Storage> storage;
};
} // namespace lang
