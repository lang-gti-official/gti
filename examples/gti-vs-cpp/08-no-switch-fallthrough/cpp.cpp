int classify(int value) {
  int result = 0;
  switch (value) {
  case 1:
    result = 10;
  default:
    result += 1;
    break;
  }
  return result;
}

int main() { return classify(1) == 11 ? 0 : 1; }
