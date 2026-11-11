MIR now derives more of each body's loans itself instead of copying them from
semantic analysis: entry loans for receivers, captures and parameters, closure
capture dependencies, parallel-scope task loans and payload-construction loans.
Semantic analysis no longer records loan end points. Compiled programs are
unchanged.
