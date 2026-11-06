Keep formatted terminal-failure helpers out of hot callers so checked constructors can inline without losing detailed panic messages or cleanup.
