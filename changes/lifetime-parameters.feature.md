GTI supports named lifetime parameters (`lifetime a`), explicit reference regions
(`T&<a>` and `mut T&<a>`), and outlives requirements. Local inference and declaration
elision keep ordinary references and most call sites annotation-free. Borrowed
records retain their declared reference relationships through calls and returns;
lifetime contracts are checked before native representation erases the regions.
