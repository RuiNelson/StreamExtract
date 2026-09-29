#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rarftp {

// Unit of work handed from the extractor thread to the uploader thread.
struct PipeMessage {
  enum class Kind {
    EnsureDir,  // Create directory `entry` (and its parents) remotely.
    FileBegin,  // Start uploading file `entry`.
    Data,       // Next chunk of the current file.
    FileEnd,    // Current file complete: `ok` tells whether it was verified.
    End,        // No more work.
  };

  Kind kind = Kind::End;
  size_t entry = 0;
  std::vector<uint8_t> data;
  bool ok = true;
  std::string error;
};

// Bounded single-producer/single-consumer queue. Only Data payload bytes
// count towards the capacity, which is what couples the decompression speed
// to the upload speed. A message larger than the capacity is still accepted
// when the queue is empty, so progress is always possible.
class Pipe {
 public:
  explicit Pipe(size_t capacity) : capacity_(capacity) {}

  Pipe(const Pipe&) = delete;
  Pipe& operator=(const Pipe&) = delete;

  // Blocks while the queue is full. Returns false once aborted.
  bool push(PipeMessage message) {
    std::unique_lock lock(mutex_);
    const size_t size = message.data.size();
    can_push_.wait(lock, [&] { return aborted_ || buffered_ == 0 || buffered_ + size <= capacity_; });
    if (aborted_) {
      return false;
    }
    buffered_ += size;
    queue_.push_back(std::move(message));
    can_pop_.notify_one();
    return true;
  }

  // Blocks until a message is available. Returns std::nullopt once aborted.
  std::optional<PipeMessage> pop() {
    std::unique_lock lock(mutex_);
    can_pop_.wait(lock, [&] { return aborted_ || !queue_.empty(); });
    if (aborted_) {
      return std::nullopt;
    }
    PipeMessage message = std::move(queue_.front());
    queue_.pop_front();
    buffered_ -= message.data.size();
    can_push_.notify_one();
    return message;
  }

  // Wakes both sides; every later push/pop fails. Pending messages are dropped.
  void abort() {
    std::lock_guard lock(mutex_);
    aborted_ = true;
    queue_.clear();
    buffered_ = 0;
    can_push_.notify_all();
    can_pop_.notify_all();
  }

  bool aborted() const {
    std::lock_guard lock(mutex_);
    return aborted_;
  }

  size_t buffered() const {
    std::lock_guard lock(mutex_);
    return buffered_;
  }

  size_t capacity() const { return capacity_; }

  // Buffer recycling, to avoid allocating (and page-faulting) a fresh block for
  // every chunk.
  std::vector<uint8_t> acquire_buffer(size_t capacity) {
    {
      std::lock_guard lock(mutex_);
      if (!free_.empty()) {
        std::vector<uint8_t> buffer = std::move(free_.back());
        free_.pop_back();
        buffer.clear();
        return buffer;
      }
    }
    std::vector<uint8_t> buffer;
    buffer.reserve(capacity);
    return buffer;
  }

  void release_buffer(std::vector<uint8_t>&& buffer) {
    std::lock_guard lock(mutex_);
    if (free_.size() < kMaxFreeBuffers && buffer.capacity() > 0) {
      free_.push_back(std::move(buffer));
    }
  }

 private:
  static constexpr size_t kMaxFreeBuffers = 16;

  const size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable can_push_;
  std::condition_variable can_pop_;
  std::deque<PipeMessage> queue_;
  std::vector<std::vector<uint8_t>> free_;
  size_t buffered_ = 0;
  bool aborted_ = false;
};

}  // namespace rarftp
