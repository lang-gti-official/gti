# Gitea issue migration (2026-10-30)

Every issue of the frozen Gitea repository `gti-lang/gti` was copied to
`lang-gti-official/gti` on 2026-10-30, with its labels, comments and open or
closed state. Each copy starts with a "Migrated from Gitea" line that links the
original.

Issue and PR numbers cited in commits, CHANGELOG entries, tests and documents
written before 2026-10-29 are **Gitea numbers**. Use this table to find the
GitHub issue. Gitea pull requests were not migrated; their numbers resolve only
on the frozen Gitea, which is still readable.

| Gitea | GitHub | State | Title |
| --- | --- | --- | --- |
| 4 | [#15](https://github.com/lang-gti-official/gti/issues/15) | closed | Reflection field access drops evaluation of the metaobject receiver |
| 5 | [#16](https://github.com/lang-gti-official/gti/issues/16) | closed | std::meta::field<T, I>::get accepts an unrelated owner type |
| 6 | [#17](https://github.com/lang-gti-official/gti/issues/17) | closed | Returning a holder constructed from expected<Item, E> emits invalid default construction |
| 73 | [#18](https://github.com/lang-gti-official/gti/issues/18) | closed | GTI-B0001: payload enum forwarded through make_unique produces invalid concrete pack element in MIR |
| 75 | [#19](https://github.com/lang-gti-official/gti/issues/19) | closed | GTI-B0001: interface-implementing class with nested polymorphic owner fails backend vocabulary admission |
| 76 | [#20](https://github.com/lang-gti-official/gti/issues/20) | closed | GTI-B0001: make_unique of derived class with owning base and declared cleanup fails backend admission |
| 80 | [#21](https://github.com/lang-gti-official/gti/issues/21) | closed | ICE: forwarding string_view parameter to owning base constructor fails MIR stage verification |
| 81 | [#22](https://github.com/lang-gti-official/gti/issues/22) | closed | ICE: owned parameter field transfer combined with owning base initialization |
| 84 | [#23](https://github.com/lang-gti-official/gti/issues/23) | closed | ICE: address of owning pointer in indexed record cannot return expected pointer |
| 85 | [#24](https://github.com/lang-gti-official/gti/issues/24) | closed | ICE: consuming expected owning payload cannot replace class field |
| 86 | [#25](https://github.com/lang-gti-official/gti/issues/25) | closed | ICE: direct truth test of expected pointer call loses failure Invoke |
| 88 | [#26](https://github.com/lang-gti-official/gti/issues/26) | closed | ICE replacing owning field through a non-failing reference-returning getter |
| 89 | [#27](https://github.com/lang-gti-official/gti/issues/27) | closed | ICE: literal alternative in owner-dependent borrowed return lacks escaping loan |
| 92 | [#28](https://github.com/lang-gti-official/gti/issues/28) | closed | ICE passing consuming Expected owning payload to a raw-pointer method receiver |
| 93 | [#29](https://github.com/lang-gti-official/gti/issues/29) | closed | ICE assigning an owning constructor result whose input borrows a view |
| 96 | [#30](https://github.com/lang-gti-official/gti/issues/30) | closed | ICE: owning argument followed by conditional input in instance method call |
| 98 | [#31](https://github.com/lang-gti-official/gti/issues/31) | closed | Defaulted derived constructor bypasses owning base initialization and emits invalid native construction |
| 99 | [#32](https://github.com/lang-gti-official/gti/issues/32) | closed | ICE: consuming method result as expected owning return payload |
| 101 | [#33](https://github.com/lang-gti-official/gti/issues/33) | closed | Non-failing base rollback emits an undeclared cleanup failure record |
| 104 | [#34](https://github.com/lang-gti-official/gti/issues/34) | closed | ICE: checked class payload copied into a constructor returning expected |
| 105 | [#35](https://github.com/lang-gti-official/gti/issues/35) | closed | ICE constructing unique owner with a default class-valued constructor argument |
| 107 | [#36](https://github.com/lang-gti-official/gti/issues/36) | closed | ICE copying a nested passive class parameter into a constructor field |
| 109 | [#37](https://github.com/lang-gti-official/gti/issues/37) | closed | ICE: consuming owning expected result of a virtual call into a return value |
| 110 | [#38](https://github.com/lang-gti-official/gti/issues/38) | closed | ICE forwarding constructor arguments through a pack followed by omitted defaults |
| 114 | [#39](https://github.com/lang-gti-official/gti/issues/39) | closed | ICE: constructor combines passive record and owning record parameters |
| 116 | [#40](https://github.com/lang-gti-official/gti/issues/40) | closed | ICE replacing an owning field through a mutable reference parameter |
| 119 | [#41](https://github.com/lang-gti-official/gti/issues/41) | closed | HIR ICE: omitted default argument on out-of-line method definition |
| 122 | [#42](https://github.com/lang-gti-official/gti/issues/42) | closed | ICE replacing an owning field through a returned raw pointer |
| 123 | [#43](https://github.com/lang-gti-official/gti/issues/43) | closed | MIR ICE retaining shared getter result in scoped worker with independent output borrow |
| 141 | [#44](https://github.com/lang-gti-official/gti/issues/44) | closed | Generated C++ identifiers depend on where the source and standard library live |
| 169 | [#45](https://github.com/lang-gti-official/gti/issues/45) | closed | Conformance block header problems are reported again for each operation |
| 171 | [#46](https://github.com/lang-gti-official/gti/issues/46) | closed | Constructor field initializers store dynamic string views without an owner dependency |
| 194 | [#47](https://github.com/lang-gti-official/gti/issues/47) | open | Success values of expected over move-only payload enums are copied or rejected |
| 195 | [#48](https://github.com/lang-gti-official/gti/issues/48) | open | GTI-B0001 reading a nested field of a call result (build(1).inner.id) |
| 196 | [#49](https://github.com/lang-gti-official/gti/issues/49) | open | GTI-B0001 reading a field of a construction from a call result (Holder(build(5)).p.id) |
| 197 | [#50](https://github.com/lang-gti-official/gti/issues/50) | open | GTI-B0001 for a lambda class result through a generic callable parameter (Plain v = f(2)) |
| 198 | [#51](https://github.com/lang-gti-official/gti/issues/51) | open | Borrowed argument before a conditional: joined temporary receiver and later temporary argument stop with GTI-B0001 |
| 199 | [#52](https://github.com/lang-gti-official/gti/issues/52) | open | Borrowed argument before a conditional: owning call result by value and consuming receiver stop with GTI-B0001 |
| 200 | [#53](https://github.com/lang-gti-official/gti/issues/53) | open | Support a local copy of a call relaying a std::string_view& parameter |
| 203 | [#54](https://github.com/lang-gti-official/gti/issues/54) | closed | Dynamic-index reads of class-typed fixed-array elements stop with GTI-B0001 |
| 204 | [#55](https://github.com/lang-gti-official/gti/issues/55) | open | GTI-B0001 indexing a fresh array value (make(1)[0], std::move(values)[0], make_row(1).cells[0]) |
| 209 | [#56](https://github.com/lang-gti-official/gti/issues/56) | closed | ICE when propagating a factory-returned payload enum after temporary-borrow reconciliation |
| 215 | [#57](https://github.com/lang-gti-official/gti/issues/57) | open | Payload candidate ICE: complete constructor facts lost in native-record container fixture |
| 216 | [#58](https://github.com/lang-gti-official/gti/issues/58) | open | Payload candidate regression: owning errors incorrectly extend borrowed-result lifetimes and cause MIR ICE |
| 217 | [#59](https://github.com/lang-gti-official/gti/issues/59) | open | ICE: direct extern-C record return used as a payload enum argument |
| 219 | [#60](https://github.com/lang-gti-official/gti/issues/60) | closed | Reference payload consolidation: nested local constructor reaches backend ICE |
| 220 | [#61](https://github.com/lang-gti-official/gti/issues/61) | closed | Borrowed payload scalar result: owner write reaches MIR ICE instead of source borrow diagnostic |
| 222 | [#62](https://github.com/lang-gti-official/gti/issues/62) | closed | Optional promotion: zero-operand terminal failure crashes native preflight |
| 224 | [#63](https://github.com/lang-gti-official/gti/issues/63) | closed | Borrowed text in source payload wrappers reaches MIR ICE instead of a source diagnostic |
| 225 | [#64](https://github.com/lang-gti-official/gti/issues/64) | closed | ICE: passive borrowed class copied into payload enum loses lifecycle and origin proof |
| 226 | [#65](https://github.com/lang-gti-official/gti/issues/65) | closed | Native callable overlay emits void for dependent generic enum fields |
| 227 | [#66](https://github.com/lang-gti-official/gti/issues/66) | closed | Borrowed payload-enum copy accepted by semantics fails MIR loan identity lowering |
| 233 | [#67](https://github.com/lang-gti-official/gti/issues/67) | open | ICE: consuming expected<string, E>.value_or reaches unsupported backend representation |
| 234 | [#68](https://github.com/lang-gti-official/gti/issues/68) | closed | Safe expected error payload accepts a view into a destroyed local string |
| 265 | [#69](https://github.com/lang-gti-official/gti/issues/69) | open | Deferred helper probing exhausted application memory during recursive assertion tests |
| 270 | [#70](https://github.com/lang-gti-official/gti/issues/70) | open | Store through a mut reference field of a local carrier emits an undeclared C++ place |
| 273 | [#71](https://github.com/lang-gti-official/gti/issues/71) | open | Generic owners storing a borrowed carrier after another field stop with GTI-B0001 |
| 274 | [#72](https://github.com/lang-gti-official/gti/issues/72) | open | Borrowed carriers that own resources stop with GTI-B0001 after a fallible field |
| 275 | [#73](https://github.com/lang-gti-official/gti/issues/73) | open | A class holding a deferred-storage class after a fallible field stops with GTI-B0001 |
| 286 | [#74](https://github.com/lang-gti-official/gti/issues/74) | open | MIR ICE: returning a resource_vector built from a resource_ref parameter (was GTI-S2085) |
| 287 | [#75](https://github.com/lang-gti-official/gti/issues/75) | open | gti_docgen publishes conformance-block functions under a generated std::__gti_conformance_<hash> namespace |
| 288 | [#76](https://github.com/lang-gti-official/gti/issues/76) | open | Misleading GTI-S2029 for a qualified static call on std::vector<T> (application classes get GTI-S2039) |
| 291 | [#77](https://github.com/lang-gti-official/gti/issues/77) | closed | Generic deferred expansion leaves dangling AST references during MIR lowering |
| 292 | [#78](https://github.com/lang-gti-official/gti/issues/78) | open | Stores inside a staged receiver or callee window stop with GTI-B0001 |
| 293 | [#79](https://github.com/lang-gti-official/gti/issues/79) | open | Discarded move-only stores in a comma or conditional arm stop with GTI-B0001 |
| 294 | [#80](https://github.com/lang-gti-official/gti/issues/80) | open | Borrowing an assignment or conditional class result as a T& argument is GTI-S2017 |
| 296 | [#81](https://github.com/lang-gti-official/gti/issues/81) | closed | Generated default-constructor temporaries fail checked call-argument MIR verification |
| 311 | [#82](https://github.com/lang-gti-official/gti/issues/82) | open | RT-11 follow-up: keep GCC's code shape for payload-enum ordinals |
| 321 | [#83](https://github.com/lang-gti-official/gti/issues/83) | open | Constructing a carrier class directly from a call's returned reference fails MIR construction |
| 322 | [#84](https://github.com/lang-gti-official/gti/issues/84) | open | GTI-B0001: a mut auto& payload binding beside a by-value binding in one arm |
| 323 | [#85](https://github.com/lang-gti-official/gti/issues/85) | open | Nested payload-pattern references cannot be returned (GTI-S2017 local child reborrow) |
| 330 | [#86](https://github.com/lang-gti-official/gti/issues/86) | closed | PR #259 regression: reference-valued pack call inputs cause MIR pointee ICE |
| 331 | [#87](https://github.com/lang-gti-official/gti/issues/87) | closed | Discarded owning macro result skips source cleanup effects and triggers MIR ICE |
| 332 | [#88](https://github.com/lang-gti-official/gti/issues/88) | closed | Macro-result persistent initializers expose eager-order ICE and missing slot cleanup |
