The language server now also marks the offending line inside a macro body: an
error raised while expanding a macro defined in the open file gets an
informational companion at that line, pointing back to the invocation, for
editors that do not display related information.
