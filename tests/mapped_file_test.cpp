#include "platform/mapped_file.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {
class MappingTest : public ::testing::Test {
 protected:
  // CTest's working directory is the preset's build tree.
  std::filesystem::path path = std::filesystem::path(u8"mapping-\u03bb-\u65e5.itch");
  void TearDown() override {
    std::error_code error;
    std::filesystem::remove(path, error);
  }
};
TEST_F(MappingTest, BinaryDataAndRepeatedLifetime) {
  const std::string expected("a\0\xffz", 4);
  {
    std::ofstream out(path, std::ios::binary);
    out.write(expected.data(), 4);
  }
  for (int repeat = 0; repeat < 100; ++repeat) {
    me::platform::MappedFile file(path);
    const auto bytes = file.bytes();
    ASSERT_EQ(bytes.size(), 4U);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), expected);
  }
  EXPECT_TRUE(std::filesystem::remove(path));
}
TEST_F(MappingTest, EmptyFile) {
  {
    std::ofstream out(path, std::ios::binary);
  }
  me::platform::MappedFile file(path);
  EXPECT_TRUE(file.bytes().empty());
}
TEST_F(MappingTest, MissingAndDirectoryAreErrors) {
  EXPECT_THROW(me::platform::MappedFile file(path), std::system_error);
  EXPECT_THROW(me::platform::MappedFile file(std::filesystem::current_path()), std::system_error);
}
}  // namespace
