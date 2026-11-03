Fix an internal compiler error when runtime `size()` or `empty()` calls observe
namespace or static-member constexpr string views, including grouped receivers
and comma expressions with side effects.
