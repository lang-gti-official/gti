#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
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

using LifetimeBinding = std::pair<Lifetime, Lifetime>;

enum class LifetimeContractOrigin : std::uint8_t {
  Declared,
  Elided,
  BodyInferred,
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

// Existential region constraints are kept separate from declaration facts.
// Projecting every path between fixed regions detects an impossible meet:
// `a >= ?r >= b` requires an independently established `a >= b`. Merely
// choosing the same inference variable never establishes that relationship.
class LifetimeInferenceConstraints {
public:
  explicit LifetimeInferenceConstraints(LifetimeBounds assumptions = {})
      : assumptions_(std::move(assumptions)) {}

  [[nodiscard]] bool require(Lifetime longer, Lifetime shorter) {
    if (!longer.specified() || !shorter.specified())
      return false;
    const LifetimeBound bound{longer, shorter};
    if (std::find(obligations_.begin(), obligations_.end(), bound) ==
        obligations_.end())
      obligations_.push_back(bound);
    return true;
  }

  // A caller may additionally supply checked local/CFG containment facts.
  // Numeric IDs and lexical declaration order are never containment proofs.
  template <typename Proven>
  [[nodiscard]] std::optional<LifetimeBound> failure(Proven proven) const {
    std::vector<Lifetime> anchors;
    for (const auto bound : obligations_)
      for (const auto region : {bound.longer, bound.shorter})
        if (region.kind != Lifetime::Kind::Inference &&
            std::find(anchors.begin(), anchors.end(), region) == anchors.end())
          anchors.push_back(region);
    for (const auto start : anchors) {
      const auto reached = reachable(start, proven);
      for (const auto end : reached)
        if (end.kind != Lifetime::Kind::Inference &&
            !assumptions_.proves(start, end) && !proven(start, end))
          return LifetimeBound{start, end};
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<LifetimeBound> failure() const {
    return failure([](Lifetime, Lifetime) { return false; });
  }

  [[nodiscard]] bool proves(Lifetime longer, Lifetime shorter) const {
    if (!longer.specified() || !shorter.specified() || failure())
      return false;
    const auto reached = reachable(longer, [](Lifetime, Lifetime) { return false; });
    return std::find(reached.begin(), reached.end(), shorter) != reached.end() ||
           assumptions_.proves(longer, shorter);
  }

  [[nodiscard]] std::span<const LifetimeBound> obligations() const {
    return obligations_;
  }

private:
  template <typename Proven>
  std::vector<Lifetime> reachable(Lifetime start, Proven proven) const {
    std::vector<Lifetime> reached{start};
    const auto add = [&](Lifetime region) {
      if (std::find(reached.begin(), reached.end(), region) == reached.end())
        reached.push_back(region);
    };
    for (std::size_t index = 0; index < reached.size(); ++index) {
      const Lifetime current = reached[index];
      for (const auto bound : obligations_) {
        if (bound.longer == current)
          add(bound.shorter);
        // Checked fixed-region facts may bridge two inferred constraints.
        if (current.kind != Lifetime::Kind::Inference &&
            bound.longer.kind != Lifetime::Kind::Inference &&
            (assumptions_.proves(current, bound.longer) ||
             proven(current, bound.longer)))
          add(bound.longer);
      }
      for (const auto bound : assumptions_.bounds())
        if (bound.longer == current)
          add(bound.shorter);
    }
    return reached;
  }

  LifetimeBounds assumptions_;
  std::vector<LifetimeBound> obligations_;
};

} // namespace lang
