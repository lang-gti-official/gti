#include "mir_lifetimes.h"

namespace lang {
MirLifetimeApplication
lowerMirLifetimeApplication(const HirLifetimeApplication &source) {
  MirLifetimeApplication result{.bindings = source.bindings,
                                .variables = source.variables,
                                .obligations = source.obligations,
                                .typeObligations = source.typeObligations,
                                .complete = source.complete};
  for (const auto &origin : source.origins)
    result.origins.push_back({.region = origin.region,
                              .key = origin.place,
                              .source = origin.source});
  return result;
}

namespace {
template <typename Facts>
bool resolveOrigins(Facts &application, const MirBody &body) {
  for (auto &origin : application.origins) {
    const MirPlace *selected = nullptr;
    if (!origin.key.valid() && origin.source == 0)
      return false;
    for (const auto &place : body.places) {
      const bool matches =
          origin.key.valid() ? place.key && *place.key == origin.key
                             : place.sourceValue == origin.source &&
                                   place.projections.empty() &&
                                   (place.root == MirPlaceRootKind::Temporary ||
                                    place.root == MirPlaceRootKind::Value);
      if (!matches)
        continue;
      // Repeated aliases of a named exact key designate the same source. A
      // temporary requires a unique materialized storage identity instead.
      if (selected && !origin.key.valid() &&
          (selected->root != place.root ||
           selected->temporary != place.temporary ||
           selected->value != place.value || selected->loan != place.loan))
        return false;
      if (!selected || place.id < selected->id)
        selected = &place;
    }
    if (!selected || selected->id == 0 ||
        (origin.place != 0 && origin.place != selected->id))
      return false;
    origin.place = selected->id;
  }
  return true;
}

template <typename HirFacts, typename MirFacts>
bool preservesFacts(const HirFacts &source, const MirFacts &lowered) {
  if (source.variables != lowered.variables ||
      source.obligations != lowered.obligations ||
      source.typeObligations != lowered.typeObligations ||
      source.complete != lowered.complete ||
      source.origins.size() != lowered.origins.size())
    return false;
  for (std::size_t index = 0; index < source.origins.size(); ++index) {
    const auto &a = source.origins[index];
    const auto &b = lowered.origins[index];
    if (a.region != b.region || a.place != b.key || a.source != b.source ||
        b.place == 0)
      return false;
  }
  return true;
}
} // namespace

bool resolveMirLifetimeOrigins(MirLifetimeApplication &application,
                               const MirBody &body) {
  return resolveOrigins(application, body);
}

bool preservesHirLifetimeApplication(const HirLifetimeApplication &source,
                                     const MirLifetimeApplication &lowered) {
  return source.bindings == lowered.bindings && preservesFacts(source, lowered);
}

MirLifetimeBody lowerMirLifetimeBody(const HirLifetimeBody &source) {
  MirLifetimeBody result{.variables = source.variables,
                         .obligations = source.obligations,
                         .typeObligations = source.typeObligations,
                         .complete = source.complete};
  for (const auto &origin : source.origins)
    result.origins.push_back({.region = origin.region,
                              .key = origin.place,
                              .source = origin.source});
  return result;
}

bool resolveMirLifetimeOrigins(MirLifetimeBody &facts, const MirBody &body) {
  return resolveOrigins(facts, body);
}

bool preservesHirLifetimeBody(const HirLifetimeBody &source,
                              const MirLifetimeBody &lowered) {
  return preservesFacts(source, lowered);
}
} // namespace lang
