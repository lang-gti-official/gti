#ifndef GTI_BORROW_CHECKER_MODE_H
#define GTI_BORROW_CHECKER_MODE_H

#include <cstdlib>

namespace lang {

// RFC 0004: MIR is the single borrow checker, borrowed-result contracts come
// from signatures alone, and MIR derives loan ends from its own CFG. Setting
// GTI_LEGACY_BORROW_CHECKER restores the retired semantic loan model, for
// comparison only, until that model is deleted.
inline bool legacyBorrowChecker() {
  static const bool legacy = std::getenv("GTI_LEGACY_BORROW_CHECKER") != nullptr;
  return legacy;
}

} // namespace lang

#endif
