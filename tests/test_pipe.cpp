#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "pipe.hpp"

using namespace streamextract;

namespace {

PipeMessage data_message(size_t size, uint8_t fill) {
  PipeMessage message;
  message.kind = PipeMessage::Kind::Data;
  message.data.assign(size, fill);
  return message;
}

}  // namespace

TEST_CASE("pipe keeps order and bounds the buffered bytes") {
  constexpr size_t kCapacity = 4096;
  constexpr int kMessages = 2000;
  Pipe pipe(kCapacity);

  std::thread producer([&] {
    for (int i = 0; i < kMessages; ++i) {
      CHECK(pipe.push(data_message(1000, static_cast<uint8_t>(i))));
    }
    PipeMessage end;
    end.kind = PipeMessage::Kind::End;
    CHECK(pipe.push(std::move(end)));
  });

  int received = 0;
  size_t max_buffered = 0;
  while (auto message = pipe.pop()) {
    if (message->kind == PipeMessage::Kind::End) {
      break;
    }
    REQUIRE(message->data.size() == 1000);
    CHECK(message->data.front() == static_cast<uint8_t>(received));
    ++received;
    max_buffered = std::max(max_buffered, pipe.buffered());
  }
  producer.join();
  CHECK(received == kMessages);
  CHECK(max_buffered <= kCapacity);
}

TEST_CASE("an oversized message is accepted when the pipe is empty") {
  Pipe pipe(10);
  CHECK(pipe.push(data_message(100, 1)));
  CHECK(pipe.buffered() == 100);
  auto message = pipe.pop();
  REQUIRE(message);
  CHECK(pipe.buffered() == 0);
}

TEST_CASE("abort wakes a blocked producer") {
  Pipe pipe(10);
  REQUIRE(pipe.push(data_message(10, 1)));
  bool pushed = true;
  std::thread producer([&] { pushed = pipe.push(data_message(10, 2)); });  // Blocks: full.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pipe.abort();
  producer.join();
  CHECK_FALSE(pushed);
  CHECK_FALSE(pipe.pop().has_value());
  CHECK(pipe.aborted());
}

TEST_CASE("abort wakes a blocked consumer") {
  Pipe pipe(10);
  bool got = true;
  std::thread consumer([&] { got = pipe.pop().has_value(); });  // Blocks: empty.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pipe.abort();
  consumer.join();
  CHECK_FALSE(got);
}

TEST_CASE("buffers are recycled") {
  Pipe pipe(10);
  std::vector<uint8_t> buffer = pipe.acquire_buffer(1024);
  CHECK(buffer.capacity() >= 1024);
  buffer.push_back(7);
  const uint8_t* storage = buffer.data();
  pipe.release_buffer(std::move(buffer));
  std::vector<uint8_t> again = pipe.acquire_buffer(1024);
  CHECK(again.empty());
  CHECK(again.data() == storage);
}

TEST_CASE("recycled buffers fit both small and large files") {
  Pipe pipe(1 << 20);
  auto large = pipe.acquire_buffer(1 << 20);
  pipe.release_buffer(std::move(large));
  auto small = pipe.acquire_buffer(4096);
  CHECK(small.capacity() >= 4096);
  CHECK(small.capacity() <= 8192);
  const uint8_t* small_storage = small.data();
  pipe.release_buffer(std::move(small));

  // A larger request must not get the tiny buffer at the back of the pool.
  auto again_large = pipe.acquire_buffer(1 << 20);
  CHECK(again_large.capacity() >= (1 << 20));
  pipe.release_buffer(std::move(again_large));
  auto again_small = pipe.acquire_buffer(4096);
  CHECK(again_small.data() == small_storage);
}

TEST_CASE("messages without data never wait for buffer space") {
  Pipe pipe(10);
  REQUIRE(pipe.push(data_message(10, 1)));  // Full.
  for (const auto kind : {PipeMessage::Kind::FileEnd, PipeMessage::Kind::EnsureDir, PipeMessage::Kind::FileBegin,
                          PipeMessage::Kind::End}) {
    PipeMessage message;
    message.kind = kind;
    CHECK(pipe.push(std::move(message)));
  }
  CHECK(pipe.buffered() == 10);
  CHECK(pipe.pop()->kind == PipeMessage::Kind::Data);
  CHECK(pipe.buffered() == 0);
  CHECK(pipe.pop()->kind == PipeMessage::Kind::FileEnd);
  CHECK(pipe.pop()->kind == PipeMessage::Kind::EnsureDir);
  CHECK(pipe.pop()->kind == PipeMessage::Kind::FileBegin);
  CHECK(pipe.pop()->kind == PipeMessage::Kind::End);
}

TEST_CASE("after an oversized message the next one waits until it is taken") {
  Pipe pipe(10);
  REQUIRE(pipe.push(data_message(100, 1)));
  std::atomic<bool> pushed{false};
  std::thread producer([&] { pushed = pipe.push(data_message(1, 2)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK_FALSE(pushed.load());  // Still blocked: 100 bytes are buffered.
  CHECK(pipe.pop()->data.size() == 100);
  producer.join();
  CHECK(pushed.load());
  CHECK(pipe.pop()->data.size() == 1);
}

TEST_CASE("abort drops pending messages and refuses new ones") {
  Pipe pipe(100);
  REQUIRE(pipe.push(data_message(10, 1)));
  REQUIRE(pipe.push(data_message(10, 2)));
  pipe.abort();
  CHECK(pipe.buffered() == 0);
  CHECK_FALSE(pipe.pop().has_value());
  CHECK_FALSE(pipe.push(data_message(1, 3)));
  pipe.abort();  // Idempotent.
  CHECK(pipe.aborted());
}

TEST_CASE("the buffer pool is bounded and keeps no empty buffers") {
  Pipe pipe(1 << 20);
  std::vector<std::vector<uint8_t>> buffers;
  for (int i = 0; i < 20; ++i) {
    buffers.push_back(pipe.acquire_buffer(1024));
  }
  std::set<const uint8_t*> retained;
  for (auto& buffer : buffers) {
    if (retained.size() < 16) {
      retained.insert(buffer.data());
    }
    pipe.release_buffer(std::move(buffer));  // The first 16 are kept.
  }
  pipe.release_buffer(std::vector<uint8_t>());  // No capacity: not worth keeping.

  std::vector<std::vector<uint8_t>> again;
  for (int i = 0; i < 17; ++i) {
    again.push_back(pipe.acquire_buffer(1024));
    CHECK(again.back().capacity() >= 1024);
    CHECK(again.back().empty());
  }
  for (int i = 0; i < 16; ++i) {
    CHECK(retained.count(again[static_cast<size_t>(i)].data()) == 1);
  }
  CHECK(retained.count(again[16].data()) == 0);  // A new allocation: the 16 kept ones are all in use.
}
