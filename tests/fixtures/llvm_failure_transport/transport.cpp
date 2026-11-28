#include "transport.h"

// The explicit cleanup schedules below stand in for fixed verified MIR edges.
// C++ destructors are deliberately not the authority for GTI secondary failure.
#if TRANSPORT_STATUS
__attribute__((noinline)) bool leaf(int *out, Channel *failure, Context *c,
                                    int value, bool fail) {
  event(c, 'D');
  if (fail) {
    *failure = originate(c);
    drop(c, 'd'); // Only the first completed subobject is live.
    return false;
  }
  event(c, 'E');
  *out = value ^ 90;
  drop(c, 'e');
  drop(c, 'd');
  return true;
}

__attribute__((noinline)) bool alternate(int *out, Channel *failure, Context *c,
                                         int value, bool fail) {
  return leaf(out, failure, c, value, fail);
}

__attribute__((noinline)) bool frame(int *out, Channel *failure, Context *c,
                                     int value, bool fail, unsigned depth,
                                     Target target) {
  event(c, static_cast<char>('A' + depth));
  int prepared = -999;
  Channel scratch;
  const bool ok =
      depth    ? frame(&prepared, &scratch, c, value, fail, depth - 1, target)
      : target ? target(&prepared, &scratch, c, value, fail)
               : leaf(&prepared, &scratch, c, value, fail);
  drop(c, static_cast<char>('a' + depth));
  if (!ok) {
    *failure = scratch;
    if (depth == 0 && c->secondary)
      emergency(c, *failure, originate(c, true));
    return false;
  }
  if (depth == 0 && c->return_failure) {
    *failure = originate(c);
    drop(c, 'r'); // Discard the prepared, not-yet-published result.
    return false;
  }
  *out = prepared;
  return true;
}
#else
namespace {
struct UnwindCleanup {
  Context *context;
  unsigned depth;
  bool active = true;

  // A fixed cleanup-only landingpad/resume schedule, not a per-frame catch,
  // rethrow, TLS query, or new primary record. The boundary owns the record.
  ~UnwindCleanup() noexcept {
    if (!active)
      return;
    drop(context, static_cast<char>('a' + depth));
    if (depth == 0 && context->secondary)
      emergency(context, context->original, originate(context, true));
  }
};
} // namespace

__attribute__((noinline)) int leaf(Context *c, int value, bool fail) {
  event(c, 'D');
  if (fail) {
    const Channel failure = originate(c);
    drop(c, 'd');
    throw failure;
  }
  event(c, 'E');
  const int result = value ^ 90;
  drop(c, 'e');
  drop(c, 'd');
  return result;
}

__attribute__((noinline)) int alternate(Context *c, int value, bool fail) {
  return leaf(c, value, fail);
}

__attribute__((noinline)) int frame(Context *c, int value, bool fail,
                                    unsigned depth, Target target) {
  event(c, static_cast<char>('A' + depth));
  UnwindCleanup cleanup{c, depth};
  const int prepared = depth    ? frame(c, value, fail, depth - 1, target)
                       : target ? target(c, value, fail)
                                : leaf(c, value, fail);
  cleanup.active = false;
  drop(c, static_cast<char>('a' + depth));
  if (depth == 0 && c->return_failure) {
    const Channel failure = originate(c);
    drop(c, 'r');
    throw failure;
  }
  return prepared;
}
#endif
