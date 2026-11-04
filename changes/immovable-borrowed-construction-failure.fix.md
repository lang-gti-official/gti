Constructing an immovable class that holds a reference, such as one with a
non-movable field and a `mut int&` member, no longer causes an internal
compiler error on the LLVM backend when the constructor can fail. A failure
during that construction now propagates and unwinds the fields already built,
instead of being routed through a later operation's failure path.
