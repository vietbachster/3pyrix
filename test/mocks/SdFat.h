#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

class FsFile {
 public:
  FsFile() = default;

  explicit operator bool() const { return isOpen_; }

  void setBuffer(const std::string& buffer) {
    buffer_ = buffer;
    position_ = 0;
    isOpen_ = true;
  }

  std::string getBuffer() const { return buffer_; }

  void close() {
    isOpen_ = false;
    position_ = 0;
  }

  size_t size() const { return buffer_.size(); }
  size_t position() const { return position_; }

  bool seek(size_t position) {
    if (position > buffer_.size()) {
      return false;
    }
    position_ = position;
    return true;
  }

  bool seekCur(int offset) {
    const auto next = static_cast<long long>(position_) + offset;
    if (next < 0 || static_cast<size_t>(next) > buffer_.size()) {
      return false;
    }
    position_ = static_cast<size_t>(next);
    return true;
  }

  int read(uint8_t* buffer, size_t length) {
    if (!isOpen_) {
      return -1;
    }

    const size_t bytesToRead = std::min(length, buffer_.size() - position_);
    if (bytesToRead == 0) {
      return 0;
    }

    std::memcpy(buffer, buffer_.data() + position_, bytesToRead);
    position_ += bytesToRead;
    return static_cast<int>(bytesToRead);
  }

  int read(char* buffer, size_t length) { return read(reinterpret_cast<uint8_t*>(buffer), length); }
  int read(void* buffer, size_t length) { return read(static_cast<uint8_t*>(buffer), length); }

  size_t write(uint8_t byte) {
    if (!isOpen_) {
      return 0;
    }

    if (position_ >= buffer_.size()) {
      buffer_.resize(position_ + 1);
    }
    buffer_[position_++] = static_cast<char>(byte);
    return 1;
  }

  size_t write(const uint8_t* buffer, size_t length) {
    if (!isOpen_) {
      return 0;
    }

    if (position_ + length > buffer_.size()) {
      buffer_.resize(position_ + length);
    }
    std::memcpy(buffer_.data() + position_, buffer, length);
    position_ += length;
    return length;
  }

 private:
  std::string buffer_;
  size_t position_ = 0;
  bool isOpen_ = false;
};
