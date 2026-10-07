#undef NDEBUG // This target-facts test must also execute checks in Release.
#include "gti/target.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

int main() {
  using C = lang::NativeCScalarKind;
  using V = lang::TargetScalarKind;
  for (const char *triple :
       {"x86_64-apple-macos", "arm64-apple-macos", "x86_64-unknown-linux-gnu",
        "arm64-unknown-linux-gnu", "x86_64-pc-windows-msvc",
        "arm64-pc-windows-msvc"}) {
    const auto target = lang::parseTargetTriple(triple);
    assert(target);
    const bool windows = target->os == "windows";
    const auto scalar = [&](C kind, C identity, V value, unsigned size) {
      const auto info = target->nativeCScalar(kind);
      assert(info && info->declarationKind == identity &&
             info->valueKind == value);
      assert(info->layout.sizeBytes == size &&
             info->layout.abiAlignmentBytes == size);
    };
    scalar(C::Bool, C::Bool, V::Bool, 1);
    scalar(C::Char, C::Char,
           target->os == "linux" && target->arch == "arm64" ? V::UInt8
                                                            : V::Int8,
           1);
    scalar(C::SignedChar, C::SignedChar, V::Int8, 1);
    scalar(C::UnsignedChar, C::UnsignedChar, V::UInt8, 1);
    scalar(C::Short, C::Short, V::Int16, 2);
    scalar(C::UnsignedShort, C::UnsignedShort, V::UInt16, 2);
    scalar(C::Int, C::Int, V::Int32, 4);
    scalar(C::UnsignedInt, C::UnsignedInt, V::UInt32, 4);
    scalar(C::Long, C::Long, windows ? V::Int32 : V::Int64, windows ? 4 : 8);
    scalar(C::UnsignedLong, C::UnsignedLong, windows ? V::UInt32 : V::UInt64,
           windows ? 4 : 8);
    scalar(C::LongLong, C::LongLong, V::Int64, 8);
    scalar(C::UnsignedLongLong, C::UnsignedLongLong, V::UInt64, 8);
    scalar(C::Float, C::Float, V::Float32, 4);
    scalar(C::Double, C::Double, V::Float64, 8);
    for (C kind : {C::Size, C::Uintptr}) {
      scalar(kind, windows ? C::UnsignedLongLong : C::UnsignedLong, V::UInt64,
             8);
    }
    for (C kind : {C::Ptrdiff, C::Intptr}) {
      scalar(kind, windows ? C::LongLong : C::Long, V::Int64, 8);
    }
    // Equal numeric widths cannot erase native C declaration identity.
    assert(target->nativeCScalar(C::Long)->declarationKind !=
           target->nativeCScalar(C::LongLong)->declarationKind);
    assert(target->nativeCScalar(C::Char)->declarationKind !=
           target->nativeCScalar(C::SignedChar)->declarationKind);
    assert(!target->nativeCScalar(C::None) && !target->nativeCScalar(C::Count));
    assert(!target->nativeCScalar(static_cast<C>(255)));
    assert(target->fixedCScalarIdentity(V::Int64) ==
           (target->os == "linux" ? C::Long : C::LongLong));
    assert(target->fixedCScalarIdentity(V::UInt64) ==
           (target->os == "linux" ? C::UnsignedLong : C::UnsignedLongLong));
    assert(!target->fixedCScalarIdentity(V::Char));
    assert(!target->fixedCScalarIdentity(V::Pointer));
    assert(!target->fixedCScalarIdentity(static_cast<V>(255)));
    for (unsigned index = 0; index < static_cast<unsigned>(V::Count); ++index) {
      const auto kind = static_cast<V>(index);
      if (const auto identity = target->fixedCScalarIdentity(kind)) {
        assert(target->nativeCScalar(*identity)->valueKind == kind);
      }
    }
    for (unsigned index = 1; index < static_cast<unsigned>(C::Count); ++index) {
      const auto kind = static_cast<C>(index);
      assert(lang::parseNativeCScalarKind(lang::nativeCScalarKindName(kind)) ==
             kind);
    }
  }
  assert(!lang::parseNativeCScalarKind("") &&
         !lang::parseNativeCScalarKind("long"));
  lang::TargetInfo invalid = lang::TargetInfo::host();
  invalid.dataLayout = lang::TargetDataLayout::unsupported();
  assert(!invalid.nativeCScalar(C::Int));
  assert(!invalid.fixedCScalarIdentity(V::Int32));
  invalid = lang::TargetInfo::host();
  invalid.os = "unsupported";
  assert(!invalid.nativeCScalar(C::Long));
  // Host comparison is evidence, never the implementation of cross-target
  // facts.
  const auto host = lang::TargetInfo::host();
  assert(host.nativeCScalar(C::Long)->layout.sizeBytes == sizeof(long));
  assert(host.nativeCScalar(C::LongLong)->layout.sizeBytes ==
         sizeof(long long));
  assert(host.nativeCScalar(C::Size)->layout.sizeBytes == sizeof(std::size_t));
  assert(host.nativeCScalar(C::Char)->valueKind ==
         (std::numeric_limits<char>::is_signed ? V::Int8 : V::UInt8));
  assert((host.nativeCScalar(C::Size)->declarationKind == C::UnsignedLong) ==
         (std::is_same_v<std::size_t, unsigned long>));
  assert((host.nativeCScalar(C::Ptrdiff)->declarationKind == C::Long) ==
         (std::is_same_v<std::ptrdiff_t, long>));
  assert((host.nativeCScalar(C::Intptr)->declarationKind == C::Long) ==
         (std::is_same_v<std::intptr_t, long>));
  assert((host.nativeCScalar(C::Uintptr)->declarationKind == C::UnsignedLong) ==
         (std::is_same_v<std::uintptr_t, unsigned long>));
  assert((host.fixedCScalarIdentity(V::Int64) == C::Long) ==
         (std::is_same_v<std::int64_t, long>));
  assert((host.fixedCScalarIdentity(V::UInt64) == C::UnsignedLong) ==
         (std::is_same_v<std::uint64_t, unsigned long>));
}
