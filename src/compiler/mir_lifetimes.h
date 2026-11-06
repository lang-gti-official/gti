#pragma once

#include "gti/mir.h"

namespace lang {
[[nodiscard]] MirLifetimeApplication
lowerMirLifetimeApplication(const HirLifetimeApplication &);
// Mapping is provenance preservation, not an outlives/containment proof.
[[nodiscard]] bool resolveMirLifetimeOrigins(MirLifetimeApplication &,
                                             const MirBody &);
[[nodiscard]] bool
preservesHirLifetimeApplication(const HirLifetimeApplication &,
                                const MirLifetimeApplication &);
[[nodiscard]] MirLifetimeBody lowerMirLifetimeBody(const HirLifetimeBody &);
[[nodiscard]] bool resolveMirLifetimeOrigins(MirLifetimeBody &,
                                             const MirBody &);
[[nodiscard]] bool preservesHirLifetimeBody(const HirLifetimeBody &,
                                            const MirLifetimeBody &);

} // namespace lang
