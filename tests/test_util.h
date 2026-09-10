#pragma once
#include <iostream>
#include <string>

// Minimal assertion helpers -- the project has no test framework dependency
// and these tests are integration checks, not unit-test sprawl.
namespace tst {

inline int failures = 0;

inline void check(bool ok, const std::string& what) {
  std::cout << (ok ? "  pass  " : "  FAIL  ") << what << "\n";
  if (!ok) ++failures;
}

inline void note(const std::string& text) {
  std::cout << "        " << text << "\n";
}

inline void section(const std::string& title) {
  std::cout << "\n== " << title << "\n";
}

inline int report() {
  std::cout << "\n"
            << (failures == 0 ? "all checks passed"
                              : std::to_string(failures) + " CHECK(S) FAILED")
            << "\n";
  return failures == 0 ? 0 : 1;
}

}  // namespace tst
