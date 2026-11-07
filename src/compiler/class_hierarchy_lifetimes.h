#pragma once

#include "gti/lifetime_types.h"

#include <algorithm>
#include <optional>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lang {

// A structural view through exact declared base edges. These substitutions
// preserve fixed regions and scoped callable binders; they grant no lifetime,
// access, storage or dispatch permission by themselves.
template <typename Instance> struct ClassHierarchyLifetimeView {
  const Instance *instance = nullptr;
  SemanticType type;
  std::vector<LifetimeBinding> bindings;
  std::vector<std::size_t> path;
};

template <typename Program>
using HierarchyClassInstance = std::remove_cvref_t<
    decltype(std::declval<const Program &>().classInstances().front())>;

template <typename Program>
std::optional<ClassHierarchyLifetimeView<HierarchyClassInstance<Program>>>
classHierarchyLifetimeView(const Program &program, std::size_t identity,
                          const SemanticType &type) {
  const auto &classes = program.classInstances();
  if (!identity || identity > classes.size()) return std::nullopt;
  const auto &instance = classes[identity - 1];
  if (instance.id != identity || type.kind != SemanticType::Class ||
      type.classId != instance.declaration ||
      instance.type.kind != SemanticType::Class ||
      instance.type.classId != instance.declaration)
    return std::nullopt;
  const auto bindings = matchUniversalTypeLifetimes(
      instance.type, type, instance.lifetimeContract.parameters);
  if (!bindings) return std::nullopt;
  return ClassHierarchyLifetimeView<HierarchyClassInstance<Program>>{
      &instance, type, *bindings, {}};
}

template <typename Program>
std::optional<ClassHierarchyLifetimeView<HierarchyClassInstance<Program>>>
classHierarchyPathView(const Program &program, std::size_t root,
                       const SemanticType &type,
                       const std::vector<std::size_t> &path) {
  auto view = classHierarchyLifetimeView(program, root, type);
  std::unordered_set<std::size_t> seen{root};
  if (path.size() > 256) return std::nullopt;
  for (const auto next : path) {
    if (!view || !seen.insert(next).second) return std::nullopt;
    std::optional<SemanticType> selected;
    for (const auto &base : view->instance->bases) {
      if (base.instance != next) continue;
      if (selected) return std::nullopt;
      selected = substituteTypeLifetimes(base.type, view->bindings);
      if (!selected) return std::nullopt;
    }
    if (!selected) return std::nullopt;
    view = classHierarchyLifetimeView(program, next, *selected);
  }
  if (view) view->path = path;
  return view;
}

template <typename Program, typename Matches>
std::optional<ClassHierarchyLifetimeView<HierarchyClassInstance<Program>>>
findUniqueClassHierarchyView(const Program &program, std::size_t root,
                             const SemanticType &type, const Matches &matches) {
  using View = ClassHierarchyLifetimeView<HierarchyClassInstance<Program>>;
  std::optional<View> selected;
  std::size_t remaining = 4096;
  const auto visit = [&](const auto &self, const View &view,
                         std::unordered_set<std::size_t> &ancestors) -> bool {
    if (remaining == 0 || view.path.size() > 256 ||
        !ancestors.insert(view.instance->id).second)
      return false;
    --remaining;
    if (matches(view)) {
      if (selected) return false;
      selected = view;
    }
    std::unordered_set<std::size_t> edges;
    for (const auto &base : view.instance->bases) {
      if (!edges.insert(base.instance).second) return false;
      const auto actual = substituteTypeLifetimes(base.type, view.bindings);
      auto child = actual ? classHierarchyLifetimeView(program, base.instance, *actual)
                          : std::nullopt;
      if (!child) return false;
      child->path = view.path;
      child->path.push_back(base.instance);
      if (!self(self, *child, ancestors)) return false;
    }
    ancestors.erase(view.instance->id);
    return true;
  };
  const auto initial = classHierarchyLifetimeView(program, root, type);
  std::unordered_set<std::size_t> ancestors;
  return initial && visit(visit, *initial, ancestors) ? selected : std::nullopt;
}

// Rebase owner regions through the selected hierarchy view. Only the remaining
// independent invocation binders may be alpha-renamed when signatures compare.
template <typename Instance, typename Function>
std::optional<SemanticType> hierarchyMemberSignature(
    const ClassHierarchyLifetimeView<Instance> &view, const Function &function) {
  if (function.owner != view.instance->id) return std::nullopt;
  const auto bindings = matchUniversalTypeLifetimes(
      function.ownerType, view.type, function.lifetimeContract.parameters);
  if (!bindings) return std::nullopt;
  auto signature = SemanticType::functionSignature(
      function.returnType, function.parameterTypes);
  const auto actual = substituteTypeLifetimes(std::move(signature), *bindings);
  if (!actual) return std::nullopt;
  signature = *actual;
  for (const auto parameter : function.lifetimeContract.parameters)
    if (std::none_of(bindings->begin(), bindings->end(),
                     [&](const auto &binding) { return binding.first == parameter; }))
      signature.quantifiedLifetimes.push_back(parameter.id);
  return signature;
}

} // namespace lang
