#include <cstdio>

#include "check.h"

int main() {
  int failed_cases = 0;
  for (const auto& c : check::cases()) {
    int before = check::failures();
    c.fn();
    bool ok = check::failures() == before;
    std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", c.name);
    if (!ok) ++failed_cases;
  }
  std::printf("\n%zu tests, %d failed, %d failed checks\n", check::cases().size(), failed_cases,
              check::failures());
  return failed_cases == 0 ? 0 : 1;
}
