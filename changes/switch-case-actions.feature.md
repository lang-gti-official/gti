Add an editor action to insert missing payload-enum switch cases as TODO
scaffolds, preserving existing cases. Switch patterns that name an enum from a
module that is not imported now explain the visibility problem and suggest the
required import instead of saying the case is not an enum variant.
