// URL slugs for members whose names are operators. cppreference spells
// these as operator%3D%3D in its own URLs; readable names are kinder and
// stable.

const OPERATOR_SLUGS: Record<string, string> = {
  "operator==": "operator_eq",
  "operator!=": "operator_ne",
  "operator<": "operator_lt",
  "operator<=": "operator_le",
  "operator>": "operator_gt",
  "operator>=": "operator_ge",
  "operator[]": "operator_at",
  "operator()": "operator_call",
  "operator*": "operator_deref",
  "operator->": "operator_arrow",
  "operator++": "operator_inc",
  "operator=": "operator_assign",
  "operator bool": "operator_bool",
};

export function memberSlug(name: string): string {
  if (OPERATOR_SLUGS[name]) return OPERATOR_SLUGS[name];
  if (name.startsWith("~")) return "destructor";
  return name;
}
