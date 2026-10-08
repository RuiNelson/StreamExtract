#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "archive_fixtures.hpp"
#include "logger.hpp"
#include "plan.hpp"
#include "temp_dir.hpp"
#include "util/remote_path.hpp"

using namespace streamextract;
using test_support::TempDir;

namespace {

ArchiveEntry make_entry(std::string name, EntryKind kind = EntryKind::File, uint64_t size = 0) {
  ArchiveEntry entry;
  entry.name = std::move(name);
  entry.kind = kind;
  entry.size = size;
  return entry;
}

// Warnings and errors logged so far that contain `text`.
size_t problems_with(const Logger& log, const std::string& text) {
  size_t count = 0;
  for (const auto& line : log.problems()) {
    if (line.text.find(text) != std::string::npos) {
      ++count;
    }
  }
  return count;
}

}  // namespace

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

TEST_CASE("the planner uploads files, creates directories and explains every entry it ignores") {
  Logger log;
  Planner planner(ArchiveFormat::Zip, "/upload", log);

  const PlannedEntry file = planner.plan(make_entry("dir/a.txt", EntryKind::File, 3));
  CHECK(file.action == PlannedEntry::Action::Upload);
  CHECK(file.relative == "dir/a.txt");
  CHECK(file.remote == "/upload/dir/a.txt");
  CHECK(file.entry.size == 3);
  CHECK(file.resume_offset == 0);
  CHECK_FALSE(file.delete_before_upload);
  CHECK_FALSE(file.excluded);

  const PlannedEntry dir = planner.plan(make_entry("dir/", EntryKind::Directory));
  CHECK(dir.action == PlannedEntry::Action::MakeDir);
  CHECK(dir.relative == "dir");
  CHECK(dir.remote == "/upload/dir");
  CHECK(log.problems().empty());

  struct Case {
    EntryKind kind;
    const char* warning;
  };
  for (const Case& c :
       {Case{EntryKind::Symlink, "skipping symbolic link \"dir/x\""},
        Case{EntryKind::Hardlink, "skipping hard link \"dir/x\""}, Case{EntryKind::FileCopy, "(rar -oi)"},
        Case{EntryKind::Special, "skipping special file \"dir/x\""}}) {
    CAPTURE(std::string(c.warning));
    const size_t before = log.problems().size();
    const PlannedEntry ignored = planner.plan(make_entry("dir/x", c.kind, 7));
    CHECK(ignored.action == PlannedEntry::Action::Ignore);
    CHECK_FALSE(ignored.excluded);
    CHECK(log.problems().size() == before + 1);
    CHECK(problems_with(log, c.warning) == 1);
  }
}

TEST_CASE("the planner sanitizes unsafe names and ignores unusable ones") {
  Logger log;
  Planner planner(ArchiveFormat::Zip, "/upload", log);

  const PlannedEntry traversal = planner.plan(make_entry("../../etc/passwd", EntryKind::File, 1));
  CHECK(traversal.action == PlannedEntry::Action::Upload);
  CHECK(traversal.relative == "etc/passwd");
  CHECK(traversal.remote == "/upload/etc/passwd");
  CHECK(problems_with(log, "\"../../etc/passwd\" points outside the destination directory") == 1);

  const PlannedEntry control = planner.plan(make_entry("bad\nname.txt", EntryKind::File, 1));
  CHECK(control.relative == "bad_name.txt");
  CHECK(problems_with(log, "contains control characters; using \"bad_name.txt\"") == 1);

  const PlannedEntry unusable = planner.plan(make_entry("a/..", EntryKind::File, 1));
  CHECK(unusable.action == PlannedEntry::Action::Ignore);
  CHECK(unusable.remote.empty());
  CHECK(problems_with(log, "unusable name") == 1);

  // A directory entry for the destination itself is harmless.
  const size_t before = log.problems().size();
  for (const char* root : {"./", "/", ""}) {
    CAPTURE(std::string(root));
    const PlannedEntry planned = planner.plan(make_entry(root, EntryKind::Directory));
    CHECK(planned.action == PlannedEntry::Action::Ignore);
    CHECK(planned.excluded);  // Not counted as "not uploaded".
  }
  CHECK(log.problems().size() == before);
}

TEST_CASE("a root directory entry such as ./ is not counted as ignored") {
  Logger log;
  ArchiveListing listing;
  listing.format = ArchiveFormat::Tar;
  listing.entries = {make_entry("./", EntryKind::Directory), make_entry("./docs/", EntryKind::Directory),
                     make_entry("./docs/a.txt", EntryKind::File, 5)};
  const TransferPlan plan = build_plan("x.tar", listing, "/upload", log);
  CHECK(plan.upload_files == 1);
  CHECK(plan.upload_bytes == 5);
  CHECK(plan.ignored == 0);
  CHECK(log.problems().empty());

  // An unusable non-directory name is still reported and counted.
  listing.entries.push_back(make_entry("a/..", EntryKind::File, 1));
  const TransferPlan other = build_plan("x.tar", listing, "/upload", log);
  CHECK(other.ignored == 1);
  CHECK(problems_with(log, "unusable name") == 1);
}

TEST_CASE("the planner warns about duplicate names and names that differ only in case") {
  Logger log;
  Planner planner(ArchiveFormat::Zip, "/upload", log);
  CHECK(planner.plan(make_entry("a.txt", EntryKind::File, 1)).action == PlannedEntry::Action::Upload);
  CHECK(planner.plan(make_entry("a.txt", EntryKind::File, 2)).action == PlannedEntry::Action::Upload);
  CHECK(problems_with(log, "\"a.txt\" appears more than once in the archive") == 1);

  CHECK(planner.plan(make_entry("A.TXT", EntryKind::File, 1)).action == PlannedEntry::Action::Upload);
  CHECK(problems_with(log, "\"a.txt\" and \"A.TXT\" differ only in letter case") == 1);

  planner.plan(make_entry("Docs/", EntryKind::Directory));
  planner.plan(make_entry("docs/", EntryKind::Directory));
  CHECK(problems_with(log, "\"Docs\" and \"docs\" differ only in letter case") == 1);

  // Repeated directory entries are not duplicates, and ignored entries collide with nothing.
  const size_t before = log.problems().size();
  planner.plan(make_entry("Docs/", EntryKind::Directory));
  planner.plan(make_entry("b/", EntryKind::Directory));
  planner.plan(make_entry("b/", EntryKind::Directory));
  CHECK(log.problems().size() == before);
  planner.plan(make_entry("B.txt", EntryKind::Symlink));
  planner.plan(make_entry("b.TXT", EntryKind::Symlink));
  CHECK(log.problems().size() == before + 2);  // Only the two link warnings.
  CHECK(problems_with(log, "letter case") == 2);
}

TEST_CASE("only non-RAR names (and RAR names on Windows) use backslashes as separators") {
  Logger log;
  Planner rar(ArchiveFormat::Rar, "/upload", log);
  Planner zip(ArchiveFormat::Zip, "/upload", log);
  // UnRAR gives native separators: on POSIX a backslash is part of the file name.
  CHECK(rar.plan(make_entry("dir\\file.txt")).relative ==
        (kNativeWindowsPaths ? "dir/file.txt" : "dir\\file.txt"));
  CHECK(zip.plan(make_entry("dir\\file.txt")).relative == "dir/file.txt");
  CHECK(zip.plan(make_entry("C:\\dir\\file.txt")).relative == "dir/file.txt");
}

TEST_CASE("plan totals exclude resumed prefixes, directories and entries outside the extraction root") {
  TransferPlan plan;
  const auto add = [&](PlannedEntry::Action action, uint64_t size, uint64_t resume = 0, bool excluded = false) {
    PlannedEntry planned;
    planned.entry.size = size;
    planned.action = action;
    planned.resume_offset = resume;
    planned.excluded = excluded;
    plan.entries.push_back(planned);
  };
  add(PlannedEntry::Action::Upload, 100, 40);
  add(PlannedEntry::Action::Upload, 10);
  add(PlannedEntry::Action::Upload, 0);
  add(PlannedEntry::Action::Skip, 50);
  add(PlannedEntry::Action::MakeDir, 0);
  add(PlannedEntry::Action::Ignore, 5);
  add(PlannedEntry::Action::Ignore, 5, 0, true);
  plan.upload_files = 99;  // Recounting starts from zero.
  for (int pass = 0; pass < 2; ++pass) {
    plan.recount();
    CHECK(plan.upload_files == 3);
    CHECK(plan.upload_bytes == 70);
    CHECK(plan.skip_files == 1);
    CHECK(plan.skip_bytes == 50);
    CHECK(plan.ignored == 1);
  }
}

TEST_CASE("build_plan keeps the archive order, its flags and the remote root") {
  Logger log;
  ArchiveListing listing;
  listing.format = ArchiveFormat::SevenZip;
  listing.flags.solid = true;
  listing.flags.skip_decompresses = true;
  listing.entries = {make_entry("a.txt", EntryKind::File, 1), make_entry("d", EntryKind::Directory),
                     make_entry("d/b.txt", EntryKind::File, 2), make_entry("d/l", EntryKind::Symlink)};
  const TransferPlan plan = build_plan("x.7z", listing, "/", log);
  CHECK(plan.archive_path == "x.7z");
  CHECK(plan.remote_root == "/");
  CHECK(plan.extraction_root.empty());
  CHECK(plan.format == ArchiveFormat::SevenZip);
  CHECK(plan.skip_decompresses);
  CHECK_FALSE(plan.streamed);
  REQUIRE(plan.entries.size() == 4);
  CHECK(plan.entries[0].remote == "/a.txt");
  CHECK(plan.entries[1].remote == "/d");
  CHECK(plan.entries[2].remote == "/d/b.txt");
  CHECK(plan.entries[3].action == PlannedEntry::Action::Ignore);
  CHECK(plan.upload_files == 2);
  CHECK(plan.upload_bytes == 3);
  CHECK(plan.ignored == 1);
}

TEST_CASE("archive descriptions") {
  ArchiveListing listing;
  listing.format = ArchiveFormat::Zip;
  listing.entries = {make_entry("a", EntryKind::File, 1000), make_entry("b", EntryKind::File, 24),
                     make_entry("d", EntryKind::Directory), make_entry("l", EntryKind::Symlink, 99)};
  CHECK(describe_archive(listing, false) == "ZIP, 2 file(s), 1.00 KiB");
  listing.volumes = 3;
  listing.flags.solid = true;
  CHECK(describe_archive(listing, true) == "ZIP, 2 file(s), 1.00 KiB, 3 volumes, solid, encrypted");

  ArchiveListing streamed;
  streamed.format = ArchiveFormat::Tar;
  streamed.flags.stream_only = true;
  streamed.flags.compression = "gzip";
  CHECK(describe_archive(streamed, false) ==
        "tar (gzip), read as it is uploaded: each file is checked against the server when it is reached");

  ArchiveListing pfs;
  pfs.format = ArchiveFormat::Pfs;
  pfs.flags.compression = "PFSC";
  CHECK(describe_archive(pfs, false) == "PFS (PFSC), 0 file(s), 0 B");
}

TEST_CASE("extraction roots are normalized, and only listed archives are checked up front") {
  CHECK(normalize_extraction_root(ArchiveFormat::Zip, "/") == "");
  CHECK(normalize_extraction_root(ArchiveFormat::Zip, "./a//b/") == "a/b");
  CHECK(normalize_extraction_root(ArchiveFormat::Zip, "a\\b") == "a/b");
  CHECK(normalize_extraction_root(ArchiveFormat::Zip, "") == "");
  CHECK_THROWS_AS(normalize_extraction_root(ArchiveFormat::Zip, "a/../b"), std::runtime_error);
  // The root is relative to the archive root: a leading '/' is not accepted (only a lone "/" is the root).
  CHECK_THROWS_AS(normalize_extraction_root(ArchiveFormat::Zip, "/b"), std::runtime_error);
  CHECK_THROWS_AS(normalize_extraction_root(ArchiveFormat::Zip, "/b/c"), std::runtime_error);
  CHECK_THROWS_WITH_AS(normalize_extraction_root(ArchiveFormat::Zip, "/b"), doctest::Contains("not starting with '/'"),
                       std::runtime_error);

  ArchiveListing streamed;
  streamed.format = ArchiveFormat::Tar;
  streamed.flags.stream_only = true;
  CHECK_NOTHROW(validate_extraction_root(streamed, "not/known/yet"));
  CHECK_THROWS_AS(validate_extraction_root(streamed, "../x"), std::runtime_error);
}

TEST_CASE("listing reads every header and reports what it cannot check") {
  const TempDir dir;
  Logger log;
  PasswordSource none(std::nullopt, PasswordSource::Prompt());

  const ArchiveListing zip = list_archive(dir.write("kinds.zip", fixtures::kKindsZip), none, log);
  CHECK(zip.format == ArchiveFormat::Zip);
  CHECK(zip.volumes == 1);
  CHECK(zip.entries.size() == 4);
  CHECK(log.problems().empty());

  const ArchiveListing tar = list_archive(dir.write("kinds.tar", fixtures::kKindsTar), none, log);
  CHECK(tar.entries.size() == 8);
  CHECK(problems_with(log, "tar archives have no checksum of the file contents") == 1);

  // A compressed tar is only opened: listing it would decompress all of it.
  const std::string gz = dir.write("a.tar.gz", fixtures::kTinyTarGz);
  const ArchiveListing streamed = list_archive(gz, none, log);
  CHECK(streamed.flags.stream_only);
  CHECK(streamed.entries.empty());
  CHECK(list_archive(gz, none, log, true).entries.size() == 1);  // The folder browser reads it.

  const size_t third = sizeof(fixtures::kTinyTar) / 3;
  const std::string first = dir.write("s.tar.001", fixtures::kTinyTar, third);
  dir.write("s.tar.002", fixtures::kTinyTar + third, third);
  dir.write("s.tar.003", fixtures::kTinyTar + 2 * third, sizeof(fixtures::kTinyTar) - 2 * third);
  CHECK(list_archive(first, none, log).volumes == 3);

  const std::string cut = dir.write("cut.rar", fixtures::kTinyRar, 38);
  CHECK_THROWS_WITH_AS(list_archive(cut, none, log),
                       ("cannot read " + cut + ": unexpected end of archive: the file is truncated").c_str(),
                       std::runtime_error);

  const std::string text = dir.write("text.zip", std::string("not an archive at all, just some text"));
  CHECK_THROWS_WITH_AS(
      list_archive(text, none, log),
      ("cannot read " + text + ": not a RAR, ZIP, 7z or tar archive, or an exFAT, PFS or UFS image").c_str(),
      std::runtime_error);
}

TEST_CASE("listing tells a missing password from a wrong one") {
  const TempDir dir;
  Logger log;
  for (const std::string& path :
       {dir.write("aes.zip", fixtures::kAesZip), dir.write("aes.7z", fixtures::kAes7z),
        dir.write("headers.7z", fixtures::kAesHeaders7z), dir.write("headers.rar", fixtures::kEncryptedRar)}) {
    CAPTURE(path);
    PasswordSource none(std::nullopt, PasswordSource::Prompt());
    CHECK_THROWS_AS(list_archive(path, none, log), ArchivePasswordRequired);
    PasswordSource wrong(std::optional<std::string>("wrong"), PasswordSource::Prompt());
    CHECK_THROWS_AS(list_archive(path, wrong, log), ArchivePasswordError);

    int prompts = 0;
    PasswordSource asked(std::nullopt, [&]() -> std::optional<std::string> {
      ++prompts;
      return "secret";
    });
    const ArchiveListing listing = list_archive(path, asked, log);
    REQUIRE(listing.entries.size() == 1);
    CHECK(listing.entries[0].name == "hello.txt");
    CHECK(prompts == 1);
  }
}
