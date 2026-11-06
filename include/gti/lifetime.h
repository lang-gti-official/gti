#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace lang {

// A semantic validity region, never a runtime value or an origin place. IDs
// belong to the immutable semantic snapshot that owns the declaration/body.
// Universal declaration parameters and existential inference variables must
// remain distinct even when their numeric IDs happen to coincide.
struct Lifetime {
  enum class Kind : std::uint8_t {
    Unspecified,
    Parameter,
    Inference,
    Local,
    Static,
  };

  Kind kind = Kind::Unspecified;
  std::uint64_t id = 0;

  [[nodiscard]] static constexpr Lifetime parameter(std::uint64_t id) {
    return {Kind::Parameter, id};
  }
  [[nodiscard]] static constexpr Lifetime inference(std::uint64_t id) {
    return {Kind::Inference, id};
  }
  [[nodiscard]] static constexpr Lifetime local(std::uint64_t id) {
    return {Kind::Local, id};
  }
  [[nodiscard]] static constexpr Lifetime staticLifetime() {
    return {Kind::Static, 0};
  }
  [[nodiscard]] constexpr bool specified() const {
    switch (kind) {
    case Kind::Parameter:
    case Kind::Inference:
    case Kind::Local:
      return id != 0;
    case Kind::Static:
      return id == 0;
    case Kind::Unspecified:
      return false;
    }
    return false;
  }
  [[nodiscard]] constexpr bool declarationRegion() const {
    return specified() &&
           (kind == Kind::Parameter || kind == Kind::Static);
  }

  friend constexpr bool operator==(Lifetime, Lifetime) = default;
};

struct LifetimeBound {
  Lifetime longer;
  Lifetime shorter;
  friend constexpr bool operator==(LifetimeBound, LifetimeBound) = default;
};

enum class LifetimeContractOrigin : std::uint8_t {
  Declared,
  Elided,
  BodyInferred,
};

// The normalized, caller-visible region interface. Declaration parameters are
// universal; no local/inferred region may be published here. Actual places,
// access permissions and drop obligations remain in the borrow contract.
struct LifetimeSignatureContract {
  std::vector<Lifetime> parameters;
  std::vector<LifetimeBound> bounds;
  Lifetime receiver;
  LifetimeContractOrigin origin = LifetimeContractOrigin::Elided;
  bool complete = false;

  [[nodiscard]] bool valid() const {
    if (!complete)
      return false;
    if (origin != LifetimeContractOrigin::Declared &&
        origin != LifetimeContractOrigin::Elided &&
        origin != LifetimeContractOrigin::BodyInferred)
      return false;
    const auto declared = [&](Lifetime region) {
      return region == Lifetime::staticLifetime() ||
             std::find(parameters.begin(), parameters.end(), region) !=
                 parameters.end();
    };
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      const Lifetime region = parameters[index];
      if (region.kind != Lifetime::Kind::Parameter || !region.specified() ||
          std::find(parameters.begin(), parameters.begin() + index, region) !=
              parameters.begin() + index)
        return false;
    }
    if (receiver != Lifetime{} &&
        (receiver.kind != Lifetime::Kind::Parameter || !declared(receiver)))
      return false;
    return std::all_of(bounds.begin(), bounds.end(), [&](LifetimeBound bound) {
      return bound.longer.declarationRegion() &&
             bound.shorter.declarationRegion() && declared(bound.longer) &&
             declared(bound.shorter);
    });
  }

  friend bool operator==(const LifetimeSignatureContract &,
                         const LifetimeSignatureContract &) = default;
};

// Only checked declaration assumptions belong here. A requested call/store
// obligation is a query, not a new assumption. Local liveness and inference
// constraints have separate owners and cannot be smuggled into this graph.
class LifetimeBounds {
public:
  [[nodiscard]] bool assume(Lifetime longer, Lifetime shorter) {
    if (!longer.declarationRegion() || !shorter.declarationRegion())
      return false;
    const LifetimeBound bound{longer, shorter};
    if (std::find(bounds_.begin(), bounds_.end(), bound) == bounds_.end())
      bounds_.push_back(bound);
    return true;
  }

  [[nodiscard]] bool proves(Lifetime longer, Lifetime shorter) const {
    if (!longer.specified() || !shorter.specified())
      return false;
    if (longer == shorter || longer.kind == Lifetime::Kind::Static)
      return true;
    std::vector<Lifetime> reached{longer};
    for (std::size_t index = 0; index < reached.size(); ++index) {
      const Lifetime current = reached[index];
      if (current.kind == Lifetime::Kind::Static)
        return true;
      for (const LifetimeBound &bound : bounds_) {
        if (bound.longer != current)
          continue;
        if (bound.shorter == shorter)
          return true;
        if (std::find(reached.begin(), reached.end(), bound.shorter) ==
            reached.end())
          reached.push_back(bound.shorter);
      }
    }
    return false;
  }

  [[nodiscard]] std::span<const LifetimeBound> bounds() const {
    return bounds_;
  }

private:
  std::vector<LifetimeBound> bounds_;
};

} // namespace lang
