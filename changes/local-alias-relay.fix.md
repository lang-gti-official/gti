Returning a call whose argument is a local reference alias, as in
`Box& local = box; return pass(local);`, now compiles when the alias is bound
to a supplier of the result's region. Before, a method gave an internal
compiler error and a free function was rejected with GTI-S2127. A result that
several parameters supply may also relay any of them through a call
(`return pass(second);`), where before that was an internal compiler error.
