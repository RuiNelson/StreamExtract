#include <optional>
#include <string>

#include <doctest/doctest.h>

#include "plan.hpp"

using namespace rarftp;

TEST_CASE("password source asks once") {
  int calls = 0;
  PasswordSource source(std::nullopt, [&]() -> std::optional<std::string> {
    ++calls;
    return "secret";
  });
  CHECK_FALSE(source.has_password());
  CHECK(calls == 0);
  CHECK(source.get() == std::optional<std::string>("secret"));
  CHECK(source.has_password());
  CHECK(source.get() == std::optional<std::string>("secret"));
  CHECK(calls == 1);
}

TEST_CASE("password source does not ask again after a refusal") {
  int calls = 0;
  PasswordSource source(std::nullopt, [&]() -> std::optional<std::string> {
    ++calls;
    return std::nullopt;
  });
  CHECK_FALSE(source.get().has_value());
  CHECK_FALSE(source.get().has_value());
  CHECK(calls == 1);
  CHECK_FALSE(source.has_password());
}

TEST_CASE("password source with a disabled prompt") {
  int calls = 0;
  PasswordSource source(std::nullopt, [&]() -> std::optional<std::string> {
    ++calls;
    return "secret";
  });
  source.disable_prompt();
  CHECK_FALSE(source.get().has_value());
  CHECK(calls == 0);
}

TEST_CASE("an explicit password is never prompted for") {
  int calls = 0;
  PasswordSource source(std::optional<std::string>("given"), [&]() -> std::optional<std::string> {
    ++calls;
    return "other";
  });
  CHECK(source.has_password());
  CHECK(source.get() == std::optional<std::string>("given"));
  CHECK(calls == 0);
}

TEST_CASE("password source without a prompt") {
  PasswordSource none(std::nullopt, PasswordSource::Prompt());
  CHECK_FALSE(none.get().has_value());
  PasswordSource given(std::optional<std::string>("given"), PasswordSource::Prompt());
  CHECK(given.get() == std::optional<std::string>("given"));
}
