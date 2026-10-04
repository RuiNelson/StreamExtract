#include <algorithm>
#include <chrono>
#include <thread>

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
      REQUIRE(pipe.push(data_message(1000, static_cast<uint8_t>(i))));
    }
    PipeMessage end;
    end.kind = PipeMessage::Kind::End;
    REQUIRE(pipe.push(std::move(end)));
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
