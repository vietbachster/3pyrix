#include "test_utils.h"

namespace TestUtils {

TestRunner::TestRunner(std::string suiteName) : suiteName_(std::move(suiteName)) {
  std::cout << "\n========================================\n";
  std::cout << "Test Suite: " << suiteName_ << "\n";
  std::cout << "========================================\n";
}

TestRunner::~TestRunner() { printSummary(); }

bool TestRunner::expectTrue(const bool condition, const std::string& testName, const std::string& message) {
  if (condition) {
    std::cout << "  PASS: " << testName << "\n";
    ++passCount_;
    return true;
  }

  std::cerr << "  FAIL: " << testName << "\n";
  if (!message.empty()) {
    std::cerr << "    " << message << "\n";
  }
  ++failCount_;
  return false;
}

bool TestRunner::expectFalse(const bool condition, const std::string& testName, const std::string& message) {
  return expectTrue(!condition, testName, message);
}

bool TestRunner::expectFloatEq(const float expected, const float actual, const std::string& testName,
                               const float epsilon) {
  if (std::fabs(expected - actual) < epsilon) {
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

void TestRunner::printSummary() const {
  std::cout << "\nSummary: " << passCount_ << " passed, " << failCount_ << " failed\n";
  std::cout << "========================================\n";
}

}  // namespace TestUtils
