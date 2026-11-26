The editor can return member suggestions as soon as the compiler has resolved
the receiver and its candidates. Large reference, highlight and rename requests
reuse each file's position index. Semantic highlighting handles long lines
without repeatedly scanning back to the start of the line.
