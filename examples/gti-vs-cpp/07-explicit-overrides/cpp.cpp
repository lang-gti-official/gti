class Reader {
public:
  virtual int read() const = 0;
  virtual ~Reader() = default;
};

class Counter : public Reader {
public:
  int read() const { return 7; }
};

int main() {
  Counter counter;
  return counter.read() == 7 ? 0 : 1;
}
