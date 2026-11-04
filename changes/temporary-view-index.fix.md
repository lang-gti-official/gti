- Indexing a view returned by a method, such as `window.view()[position(i)]`,
  no longer stops with internal compiler error GTI-B0001 when the index
  expression can fail; the view's owner stays borrowed until the view is
  destroyed on that failure path too.
