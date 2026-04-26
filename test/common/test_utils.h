#pragma once

#include <cmath>
#include <iostream>
#include <sstream>
#include <string>

namespace TestUtils {

class TestRunner {
 public:
  explicit TestRunner(std::string suiteName);
  ~TestRunner();

  bool expectTrue(bool condition, const std::string& testName, const std::string& message = "");
  bool expectFalse(bool condition, const std::string& testName, const std::string& message = "");

  template <typename T, typename U>
  bool expectEq(const T& expected, const U& actual, const std::string& testName) {
    if (expected == actual) {
      std::cout << "  PASS: " << testName << "\n";
      ++passCount_;
      return true;
    }

    std::cerr << "  FAIL: " << testName << "\n";
    std::cerr << "    Expected: " << expected << "\n";
    std::cerr << "    Actual:   " << actual << "\n";
    ++failCount_;
    return false;
  }

  bool expectFloatEq(float expected, float actual, const std::string& testName, float epsilon = 0.001f);

  bool allPassed() const { return failCount_ == 0; }

 private:
  void printSummary() const;

  std::string suiteName_;
  int passCount_ = 0;
  int failCount_ = 0;
};

}  // namespace TestUtils
