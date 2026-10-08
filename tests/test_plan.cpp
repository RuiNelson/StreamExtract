#include <optional>
#include <string>

#include <doctest/doctest.h>

#include "logger.hpp"
#include "plan.hpp"

using namespace streamextract;

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

TEST_CASE("archive description formats original bytes in the selected units") {
  ArchiveListing listing;
  ArchiveEntry entry;
  entry.name = "64 MiB.txt";
  entry.kind = EntryKind::File;
  entry.size = 10480;
  listing.entries.push_back(entry);
  CHECK(describe_archive(listing, false) == "RAR, 1 file(s), 10.2 KiB");
  CHECK(describe_archive(listing, false, ByteUnits::Si) == "RAR, 1 file(s), 10.5 kB");
}

namespace {
ArchiveListing root_listing() {
  ArchiveListing listing;
  listing.format = ArchiveFormat::Zip;
  for (const auto& name : {"a/x.txt", "a/x/", "a/y/", "b/xyz.bin", "b/nested/zero", "b2/outside"}) {
    ArchiveEntry entry;
    entry.name = name;
    entry.kind = entry.name.back() == '/' ? EntryKind::Directory : EntryKind::File;
    entry.size = entry.kind == EntryKind::File && entry.name != "b/nested/zero" ? 6 : 0;
    listing.entries.push_back(entry);
  }
  return listing;
}
}  // namespace

TEST_CASE("extraction root strips only the selected directory and keeps archive order") {
  const auto listing = root_listing();
  Logger log;
  const auto b = build_plan("test.zip", listing, "/remote/dest", log, "./b/");
  REQUIRE(b.entries.size() == listing.entries.size());
  CHECK(b.extraction_root == "b");
  CHECK(b.upload_files == 2);
  CHECK(b.upload_bytes == 6);
  CHECK(b.ignored == 0);
  for (size_t i : {0u, 1u, 2u, 5u}) {
    CHECK(b.entries[i].action == PlannedEntry::Action::Ignore);
    CHECK(b.entries[i].excluded);
    CHECK(b.entries[i].remote.empty());
  }
  CHECK(b.entries[3].entry.name == "b/xyz.bin");
  CHECK(b.entries[3].relative == "xyz.bin");
  CHECK(b.entries[3].remote == "/remote/dest/xyz.bin");
  CHECK(b.entries[4].remote == "/remote/dest/nested/zero");

  const auto a = build_plan("test.zip", listing, "/remote/dest", log, "a");
  CHECK(a.upload_files == 1);
  CHECK(a.entries[0].remote == "/remote/dest/x.txt");
  CHECK(a.entries[1].action == PlannedEntry::Action::MakeDir);
  CHECK(a.entries[1].remote == "/remote/dest/x");
  CHECK(a.entries[2].action == PlannedEntry::Action::MakeDir);
  CHECK(a.entries[2].remote == "/remote/dest/y");

  const auto empty = build_plan("test.zip", listing, "/remote/dest", log, "a/x");
  CHECK(empty.upload_files == 0);
  CHECK(empty.ignored == 0);
  CHECK(empty.entries[1].excluded);  // The selected folder itself maps to the destination.
  CHECK(log.problems().empty());
}

TEST_CASE("archive folder picker includes implicit parents and empty directories") {
  CHECK(archive_directories(root_listing()) == std::vector<std::string>{"a", "a/x", "a/y", "b", "b/nested", "b2"});
}

TEST_CASE("extraction root rejects missing directories and files with the same name") {
  Logger log;
  for (const auto& root : {"missing", "B", "b/xyz.bin", "../b", "b/../a", "/b", "b\n"}) {
    CHECK_THROWS(build_plan("test.zip", root_listing(), "/upload", log, root));
  }
  for (const auto& root : {"", "/", "."}) {
    const auto plan = build_plan("test.zip", root_listing(), "/upload", log, root);
    CHECK(plan.upload_files == 4);
    CHECK(plan.entries[3].remote == "/upload/b/xyz.bin");
  }
}

TEST_CASE("streamed extraction roots use the same mapping and detect missing folders at EOF") {
  Logger log;
  auto listing = root_listing();
  listing.flags.stream_only = true;
  listing.entries.clear();
  const auto plan = build_plan("test.tar.gz", listing, "/upload", log, "b");
  CHECK(plan.streamed);
  CHECK(plan.entries.empty());
  Planner planner(plan.format, plan.remote_root, log, plan.extraction_root);
  CHECK_FALSE(planner.found_root());
  const auto headers = root_listing().entries;
  CHECK(planner.plan(headers[0]).excluded);
  CHECK_FALSE(planner.found_root());
  CHECK(planner.plan(headers[3]).remote == "/upload/xyz.bin");
  CHECK(planner.found_root());
  CHECK(planner.plan(headers[5]).excluded);
}

TEST_CASE("extraction roots and folder picker use the sanitized archive spelling") {
  Logger log;
  ArchiveListing listing;
  listing.format = ArchiveFormat::Zip;
  ArchiveEntry entry;
  entry.name = "folder\\nested\\file.txt";
  listing.entries.push_back(entry);
  CHECK(archive_directories(listing) == std::vector<std::string>{"folder", "folder/nested"});
  CHECK(build_plan("test.zip", listing, "/upload", log, "folder\\nested").entries[0].remote == "/upload/file.txt");
  entry.name = "../folder/nested/file.txt";
  listing.entries = {entry};
  const auto plan = build_plan("test.zip", listing, "/upload", log, "folder/nested");
  CHECK(plan.entries[0].remote == "/upload/file.txt");
  CHECK(log.problems().size() == 1);
}
