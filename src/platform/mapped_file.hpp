#pragma once

#include <cstddef>
#include <filesystem>
#include <span>

namespace me::platform {
// Input must remain unchanged for the mapping's lifetime.
class MappedFile {
 public:
  explicit MappedFile(const std::filesystem::path& path);
  ~MappedFile();
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  MappedFile(MappedFile&&) = delete;
  MappedFile& operator=(MappedFile&&) = delete;
  std::span<const std::byte> bytes() const noexcept { return {data_, size_}; }

 private:
  const std::byte* data_ = nullptr;
  std::size_t size_ = 0;
};
}  // namespace me::platform
