#pragma once

#include "gti/runtime_failure.h"
#include <cstdint>

// This mirrors Abi::failureChannel(), not a public GTI or C embedding ABI.
struct Channel {
  gti_failure_record_v2 record;
  const gti_failure_artifact_descriptor_v1 *artifact;
};

struct Context {
  char trace[128]{};
  unsigned used = 0;
  std::uint64_t drops = 0;
  bool tracing = true;
  bool secondary = false;
  bool return_failure = false;
  bool caller = false;
  bool published = false;
  Channel original{};
  Channel contained{};
};

void event(Context *, char) noexcept;
void drop(Context *, char) noexcept;
Channel originate(Context *, bool secondary = false) noexcept;
void audit(const Channel &, const Context &) noexcept;
[[noreturn]] void emergency(Context *, const Channel &, const Channel &) noexcept;

#if TRANSPORT_STATUS
using Target = bool (*)(int *, Channel *, Context *, int, bool);
bool leaf(int *, Channel *, Context *, int, bool);
bool alternate(int *, Channel *, Context *, int, bool);
bool frame(int *, Channel *, Context *, int, bool, unsigned, Target);
#else
using Target = int (*)(Context *, int, bool);
int leaf(Context *, int, bool);
int alternate(Context *, int, bool);
int frame(Context *, int, bool, unsigned, Target);
#endif

extern "C" int transport_c_host(int (*callback)(void *), void *, int *);
