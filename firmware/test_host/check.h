#pragma once
// Tiny dependency-free test harness for the host build.

#include <cmath>
#include <cstdio>
#include <vector>

namespace check {
struct Case {
  const char* name;
  void (*fn)();
};
inline std::vector<Case>& cases() {
  static std::vector<Case> c;
  return c;
}
inline int& failures() {
  static int f = 0;
  return f;
}
struct Reg {
  Reg(const char* n, void (*f)()) { cases().push_back({n, f}); }
};
}  // namespace check

#define TEST(name)                                     \
  static void name();                                  \
  static check::Reg reg_##name(#name, &name);          \
  static void name()

#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::printf("    FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);        \
      ++check::failures();                                               \
    }                                                                    \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                              \
  do {                                                                                     \
    double a_ = (a), b_ = (b);                                                             \
    if (std::fabs(a_ - b_) > (tol)) {                                                      \
      std::printf("    FAIL %s:%d  %s = %g, expected %g +- %g\n", __FILE__, __LINE__, #a, a_, \
                  b_, static_cast<double>(tol));                                           \
      ++check::failures();                                                                 \
    }                                                                                      \
  } while (0)
