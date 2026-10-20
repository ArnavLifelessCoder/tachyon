#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <limits>
#include <system_error>

#include "platform/mapped_file.hpp"

namespace me::platform {
namespace {
struct Handle {
  HANDLE value;
  ~Handle() {
    if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
  }
};
[[noreturn]] void fail() {
  throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "map input");
}
}  // namespace
MappedFile::MappedFile(const std::filesystem::path& path) {
  const Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (file.value == INVALID_HANDLE_VALUE) fail();
  LARGE_INTEGER size;
  if (!GetFileSizeEx(file.value, &size)) fail();
  if (size.QuadPart < 0 ||
      static_cast<unsigned long long>(size.QuadPart) > std::numeric_limits<std::size_t>::max()) {
    throw std::system_error(std::make_error_code(std::errc::file_too_large));
  }
  size_ = static_cast<std::size_t>(size.QuadPart);
  if (size_ == 0) return;
  const Handle mapping{CreateFileMappingW(file.value, nullptr, PAGE_READONLY, 0, 0, nullptr)};
  if (!mapping.value) fail();
  data_ = static_cast<const std::byte*>(MapViewOfFile(mapping.value, FILE_MAP_READ, 0, 0, 0));
  if (!data_) fail();
}
MappedFile::~MappedFile() {
  if (data_) UnmapViewOfFile(data_);
}
}  // namespace me::platform
