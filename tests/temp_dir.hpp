// A temporary directory for a test, removed with everything in it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace test_support {

class TempDir {
 public:
  TempDir() {
    static int counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("streamextract-test-" + std::to_string(reinterpret_cast<uintptr_t>(this)) + "-" +
             std::to_string(++counter));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const { return path_; }

  std::string write(const std::string& name, const unsigned char* data, size_t size) const {
    const std::filesystem::path file = path_ / name;
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return file.string();
  }
  template <size_t N>
  std::string write(const std::string& name, const unsigned char (&data)[N]) const {
    return write(name, data, N);
  }
  std::string write(const std::string& name, const std::string& data) const {
    return write(name, reinterpret_cast<const unsigned char*>(data.data()), data.size());
  }

 private:
  std::filesystem::path path_;
};

}  // namespace test_support
