Fix C++ backend construction of owning reference carriers after earlier fields,
and of nested classes containing deferred reference fields. Preserve source
initialization order, rollback cleanup, and moved reference bindings without
requiring an invalid default value or assignment through a reference member.
