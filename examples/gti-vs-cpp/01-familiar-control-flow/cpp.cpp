int sum(int values[4]) {
  int total = 0;
  for (int index = 0; index < 4; index++) {
    total += values[index];
  }
  return total;
}

int main() {
  int values[4] = {1, 2, 3, 4};
  int total = sum(values);
  if (total == 10) {
    return 0;
  }
  return 1;
}
