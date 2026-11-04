Fix owning constructor-pack results passed to another call, preserving their
single transfer and cleanup on argument failure. Fix discarded conditional
assignment lowering so it does not create an extra owner or destroy a stored
value twice; preserve move-only assignment through comma and conditional forms
in the C++ backend.
