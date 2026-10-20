#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <limits>
#include <system_error>

#include "platform/mapped_file.hpp"

namespace me::platform {
namespace {
struct Descriptor {
  int value;
  ~Descriptor() {
    if (value >= 0) close(value);
  }
};
[[noreturn]] void fail() { throw std::system_error(errno, std::generic_category(), "map input"); }
}  // namespace
MappedFile::MappedFile(const std::filesystem::path& path) {
  const Descriptor file{open(path.c_str(), O_RDONLY | O_CLOEXEC)};
  if (file.value < 0) fail();
  struct stat info{};
  if (fstat(file.value, &info) < 0) fail();
  if (!S_ISREG(info.st_mode))
    throw std::system_error(std::make_error_code(std::errc::invalid_argument));
  if (info.st_size < 0 ||
      static_cast<std::uintmax_t>(info.st_size) > std::numeric_limits<std::size_t>::max()) {
    throw std::system_error(std::make_error_code(std::errc::file_too_large));
  }
  size_ = static_cast<std::size_t>(info.st_size);
  if (size_ == 0) return;
  const auto mapped = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, file.value, 0);
  if (mapped == MAP_FAILED) fail();
  data_ = static_cast<const std::byte*>(mapped);
}
MappedFile::~MappedFile() {
  if (data_) munmap(const_cast<std::byte*>(data_), size_);
}
}  // namespace me::platform
