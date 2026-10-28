"""Matrix 24: recursion over owned trees.

Binary search trees and n-ary trees whose children are unique_ptr owners,
built, folded and searched recursively; subtrees detached and moved between
owners; a long owned list destroyed; polymorphic expression trees; and a
reference into the tree returned out of a recursive search.
"""

PRELUDE = """#include <std/memory>
#include <std/vector>
#include <std/string>
#include <std/format>
#include <std/utility>
#include <std/atomic>
std::atomic<int32_t> live{0};
class Node {
public:
  int64_t key;
  mut std::unique_ptr<Node> left = std::unique_ptr<Node>();
  mut std::unique_ptr<Node> right = std::unique_ptr<Node>();
  Node(int64_t key) : key(key) { [[discard]] live.fetch_add(1); }
  ~Node() { [[discard]] live.fetch_sub(1); }
};
void insert(mut std::unique_ptr<Node>& slot, int64_t key) {
  if (!slot) {
    slot = std::make_unique<Node>(key);
    return;
  }
  if (key < slot->key) insert(slot->left, key);
  else insert(slot->right, key);
}
int64_t sum(std::unique_ptr<Node>& slot) {
  if (!slot) return 0;
  return slot->key + sum(slot->left) + sum(slot->right);
}
int32_t depth(std::unique_ptr<Node>& slot) {
  if (!slot) return 0;
  int32_t l = depth(slot->left);
  int32_t r = depth(slot->right);
  return 1 + (l > r ? l : r);
}
"""

CELLS = {
    "bst_fold": """int main() {
  {
    mut std::unique_ptr<Node> root = std::unique_ptr<Node>();
    int64_t keys[7] = {50, 30, 70, 20, 40, 60, 80};
    for (int64_t key : keys) insert(root, key);
    if (sum(root) != int64_t(350) || depth(root) != 3 || live.load() != 7) return 1;
  }
  if (live.load() != 0) return 2;
  std::println("ok");
  return 0;
}
""",
    "detach_subtree": """int main() {
  {
    mut std::unique_ptr<Node> root = std::unique_ptr<Node>();
    int64_t keys[5] = {5, 3, 8, 1, 4};
    for (int64_t key : keys) insert(root, key);
    std::unique_ptr<Node> branch = std::move(root->left);
    if (sum(root) != int64_t(13) || sum(branch) != int64_t(8) || live.load() != 5) return 1;
    root->left = std::unique_ptr<Node>();
    mut std::unique_ptr<Node> other = std::unique_ptr<Node>();
    insert(other, int64_t(100));
    other->left = std::move(root);
    if (sum(other) != int64_t(113) || depth(other) != 3) return 2;
  }
  if (live.load() != 0) return 3;
  std::println("ok");
  return 0;
}
""",
    "nary_tree": """class Dir {
public:
  std::string name;
  mut std::vector<std::unique_ptr<Dir>> children = std::vector<std::unique_ptr<Dir>>();
  Dir(std::string name) : name(std::move(name)) { [[discard]] live.fetch_add(1); }
  ~Dir() { [[discard]] live.fetch_sub(1); }
};
std::unique_ptr<Dir> build(int32_t level, int32_t width) {
  mut std::unique_ptr<Dir> dir = std::make_unique<Dir>(
      std::format("a directory name long enough for the heap {}", level));
  if (level > 0) {
    for (mut int32_t i = 0; i < width; i += 1) dir->children.push_back(build(level - 1, width));
  }
  return dir;
}
int32_t count(std::unique_ptr<Dir>& dir) {
  mut int32_t total = 1;
  for (auto& child : dir->children) total += count(child);
  return total;
}
int main() {
  {
    std::unique_ptr<Dir> root = build(3, 3);
    if (count(root) != 40 || live.load() != 40) return 1;
  }
  if (live.load() != 0) return 2;
  std::println("ok");
  return 0;
}
""",
    "long_list": """class Link {
public:
  int32_t value;
  mut std::unique_ptr<Link> next = std::unique_ptr<Link>();
  Link(int32_t value) : value(value) { [[discard]] live.fetch_add(1); }
  ~Link() { [[discard]] live.fetch_sub(1); }
};
int64_t total(std::unique_ptr<Link>& link) {
  if (!link) return 0;
  return int64_t(link->value) + total(link->next);
}
int main() {
  {
    mut std::unique_ptr<Link> head = std::unique_ptr<Link>();
    for (mut int32_t i = 1; i <= 500; i += 1) {
      mut std::unique_ptr<Link> link = std::make_unique<Link>(i);
      link->next = std::move(head);
      head = std::move(link);
    }
    if (total(head) != int64_t(125250) || live.load() != 500) return 1;
  }
  if (live.load() != 0) return 2;
  std::println("ok");
  return 0;
}
""",
    "polymorphic_expression": """interface Expr {
  int64_t eval();
};
class Num : public Expr {
  int64_t value;
public:
  Num(int64_t value) : value(value) { [[discard]] live.fetch_add(1); }
  ~Num() { [[discard]] live.fetch_sub(1); }
  int64_t eval() override { return this.value; }
};
class Add : public Expr {
  std::unique_ptr<Expr> lhs;
  std::unique_ptr<Expr> rhs;
public:
  Add(std::unique_ptr<Expr> lhs, std::unique_ptr<Expr> rhs)
      : lhs(std::move(lhs)), rhs(std::move(rhs)) { [[discard]] live.fetch_add(1); }
  ~Add() { [[discard]] live.fetch_sub(1); }
  int64_t eval() override { return this.lhs->eval() + this.rhs->eval(); }
};
class Mul : public Expr {
  std::unique_ptr<Expr> lhs;
  std::unique_ptr<Expr> rhs;
public:
  Mul(std::unique_ptr<Expr> lhs, std::unique_ptr<Expr> rhs)
      : lhs(std::move(lhs)), rhs(std::move(rhs)) { [[discard]] live.fetch_add(1); }
  ~Mul() { [[discard]] live.fetch_sub(1); }
  int64_t eval() override { return this.lhs->eval() * this.rhs->eval(); }
};
std::unique_ptr<Expr> chain(int32_t n) {
  if (n == 0) return std::upcast_unique<Expr, Num>(std::make_unique<Num>(int64_t(1)));
  if (n % 2 == 0)
    return std::upcast_unique<Expr, Add>(std::make_unique<Add>(
        chain(n - 1), std::upcast_unique<Expr, Num>(std::make_unique<Num>(int64_t(n)))));
  return std::upcast_unique<Expr, Mul>(std::make_unique<Mul>(
      chain(n - 1), std::upcast_unique<Expr, Num>(std::make_unique<Num>(int64_t(2)))));
}
int main() {
  {
    std::unique_ptr<Expr> e = chain(6);
    if (e->eval() != int64_t(30)) {
      std::println("{}", e->eval());
      return 1;
    }
  }
  if (live.load() != 0) return 2;
  std::println("ok");
  return 0;
}
""",
    "found_reference": """Node& find(Node& node, int64_t key) {
  if (key < node.key && node.left) return find(*node.left, key);
  if (key > node.key && node.right) return find(*node.right, key);
  return node;
}
int main() {
  mut std::unique_ptr<Node> root = std::unique_ptr<Node>();
  int64_t keys[4] = {8, 4, 12, 6};
  for (int64_t key : keys) insert(root, key);
  Node& hit = find(*root, int64_t(6));
  Node& miss = find(*root, int64_t(13));
  if (hit.key != int64_t(6) || miss.key != int64_t(12)) return 1;
  std::println("ok");
  return 0;
}
""",
}


def cells():
    for name, body in CELLS.items():
        yield name, PRELUDE + body
