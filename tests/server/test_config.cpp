/******************************************************************************
 *
 * Copyright Saab AB, 2026 (https://github.com/SafirSDK/minilog)
 *
 * Created by: Lars Hagström / lars@foldspace.nu
 *
 *******************************************************************************
 *
 * This file is part of minilog.
 *
 * minilog is released under the MIT License. See the LICENSE file in
 * the project root for full license information.
 *
 ******************************************************************************/

#define BOOST_TEST_MODULE test_config
#include "config/config.hpp"

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

using namespace minilog;

// Every text_file / jsonl_file below must be an absolute path, because that is
// what loadConfig now requires. "/tmp/f" is absolute on POSIX but not on
// Windows, where a path with a root directory and no root name is relative to
// the current drive — so the Windows build names a drive. Nothing here opens the
// files; only the string is validated.
#ifdef _WIN32
#define ABS "C:"
#else
#define ABS ""
#endif

namespace
{

// Write content to a unique temp file; return its path.
// The file is NOT automatically deleted — callers clean up if they care.
std::string write_temp(const std::string& content)
{
    static std::atomic<int> counter{0};
    const auto path = (std::filesystem::temp_directory_path() /
                       ("minilog_cfg_test_" + std::to_string(counter++) + ".ini"))
                          .string();
    std::ofstream f(path, std::ios::binary);
    BOOST_REQUIRE_MESSAGE(f, "Could not open temp file: " + path);
    f << content;
    return path;
}

struct TempFile
{
    std::string path;
    explicit TempFile(const std::string& content) : path(write_temp(content)) {}
    ~TempFile() { std::filesystem::remove(path); }
};

} // namespace

// ─── Defaults ────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(defaults)

BOOST_AUTO_TEST_CASE(struct_defaults)
{
    Config cfg;
    BOOST_TEST(cfg.host == "0.0.0.0");
    BOOST_TEST(cfg.udpPort == 514);
    BOOST_TEST(cfg.workers == 4);
    BOOST_TEST(cfg.outputs.empty());
    BOOST_TEST(!cfg.forwarding.enabled);
}

BOOST_AUTO_TEST_CASE(minimal_config_uses_defaults)
{
    TempFile tmp("[server]\n"
                 "udp_port = 5514\n"
                 "\n"
                 "[output.main]\n"
                 "text_file = " ABS "/tmp/syslog.log\n");
    Config cfg = loadConfig(tmp.path);
    BOOST_TEST(cfg.host == "0.0.0.0");
    BOOST_TEST(cfg.udpPort == 5514);
    BOOST_TEST(cfg.workers == 4);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Full valid config ────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(full_config)

BOOST_AUTO_TEST_CASE(all_fields_parsed)
{
    TempFile tmp("[server]\n"
                 "host = 127.0.0.1\n"
                 "udp_port = 5514\n"
                 "workers = 8\n"
                 "\n"
                 "[output.main]\n"
                 "text_file = " ABS "/var/log/syslog.log\n"
                 "jsonl_file = " ABS "/var/log/syslog.jsonl\n"
                 "max_size = 100MB\n"
                 "max_files = 10\n"
                 "facility = *\n"
                 "include_malformed = true\n"
                 "\n"
                 "[output.auth]\n"
                 "text_file = " ABS "/var/log/auth.log\n"
                 "max_size = 50MB\n"
                 "max_files = 5\n"
                 "facility = auth,authpriv\n"
                 "include_malformed = false\n"
                 "\n"
                 "[forwarding]\n"
                 "enabled = true\n"
                 "host = 10.0.0.5\n"
                 "port = 1514\n"
                 "max_message_size = 4096\n"
                 "facility = *\n");
    Config cfg = loadConfig(tmp.path);

    BOOST_TEST(cfg.host == "127.0.0.1");
    BOOST_TEST(cfg.udpPort == 5514);
    BOOST_TEST(cfg.workers == 8);

    BOOST_REQUIRE(cfg.outputs.size() == 2);

    const auto& main = cfg.outputs[0];
    BOOST_TEST(main.name == "main");
    BOOST_TEST(main.textFile == ABS "/var/log/syslog.log");
    BOOST_TEST(main.jsonlFile == ABS "/var/log/syslog.jsonl");
    BOOST_TEST(main.maxSize == 100ULL * 1024 * 1024);
    BOOST_TEST(main.maxFiles == 10);
    BOOST_TEST(main.facilities.empty()); // * → all
    BOOST_TEST(main.includeMalformed);

    const auto& auth = cfg.outputs[1];
    BOOST_TEST(auth.name == "auth");
    BOOST_TEST(auth.textFile == ABS "/var/log/auth.log");
    BOOST_TEST(auth.maxSize == 50ULL * 1024 * 1024);
    BOOST_TEST(auth.maxFiles == 5);
    BOOST_TEST(!auth.includeMalformed);
    BOOST_REQUIRE(auth.facilities.size() == 2);
    // auth=4, authpriv=10
    BOOST_TEST(auth.facilities[0] == 4);
    BOOST_TEST(auth.facilities[1] == 10);

    BOOST_TEST(cfg.forwarding.enabled);
    BOOST_TEST(cfg.forwarding.host == "10.0.0.5");
    BOOST_TEST(cfg.forwarding.port == 1514);
    BOOST_TEST(cfg.forwarding.maxMessageSize == 4096u);
    BOOST_TEST(cfg.forwarding.facilities.empty()); // *
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Port validation ──────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(port_validation)

BOOST_AUTO_TEST_CASE(port_zero_valid)
{
    TempFile tmp("[server]\nudp_port = 0\n\n[output.m]\ntext_file = " ABS "/tmp/f\n");
    Config cfg = loadConfig(tmp.path);
    BOOST_TEST(cfg.udpPort == 0);
}

BOOST_AUTO_TEST_CASE(port_65536_invalid)
{
    TempFile tmp("[server]\nudp_port = 65536\n\n[output.m]\ntext_file = " ABS "/tmp/f\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(port_negative_invalid)
{
    TempFile tmp("[server]\nudp_port = -1\n\n[output.m]\ntext_file = " ABS "/tmp/f\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(port_non_numeric_throws)
{
    TempFile tmp("[server]\nudp_port = abc\n\n[output.m]\ntext_file = " ABS "/tmp/f\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(port_trailing_chars_throws)
{
    // "123abc" has a valid numeric prefix but trailing non-digits — requireInt must reject it.
    TempFile tmp("[server]\nudp_port = 123abc\n\n[output.m]\ntext_file = " ABS "/tmp/f\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(workers_trailing_chars_throws)
{
    TempFile tmp("[server]\nworkers = 4x\n\n[output.m]\ntext_file = " ABS "/tmp/f\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(port_1_valid)
{
    TempFile tmp("[server]\nudp_port = 1\n\n[output.m]\ntext_file = " ABS "/tmp/f\n");
    BOOST_CHECK_NO_THROW(loadConfig(tmp.path));
}

BOOST_AUTO_TEST_CASE(port_65535_valid)
{
    TempFile tmp("[server]\nudp_port = 65535\n\n[output.m]\ntext_file = " ABS "/tmp/f\n");
    Config cfg = loadConfig(tmp.path);
    BOOST_TEST(cfg.udpPort == 65535);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Size parsing ─────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(size_parsing)

BOOST_AUTO_TEST_CASE(bytes)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nmax_size=512B\n");
    BOOST_TEST(loadConfig(tmp.path).outputs[0].maxSize == 512u);
}

BOOST_AUTO_TEST_CASE(kilobytes)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nmax_size=64KB\n");
    BOOST_TEST(loadConfig(tmp.path).outputs[0].maxSize == 64u * 1024);
}

BOOST_AUTO_TEST_CASE(megabytes)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nmax_size=100MB\n");
    BOOST_TEST(loadConfig(tmp.path).outputs[0].maxSize == 100ULL * 1024 * 1024);
}

BOOST_AUTO_TEST_CASE(gigabytes)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nmax_size=2GB\n");
    BOOST_TEST(loadConfig(tmp.path).outputs[0].maxSize == 2ULL * 1024 * 1024 * 1024);
}

BOOST_AUTO_TEST_CASE(no_unit_treated_as_bytes)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nmax_size=1024\n");
    BOOST_TEST(loadConfig(tmp.path).outputs[0].maxSize == 1024u);
}

BOOST_AUTO_TEST_CASE(unknown_unit_throws)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nmax_size=100TB\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(zero_size_means_no_rotation)
{
    // 0 is the documented way to disable rotation — config.hpp, the README and
    // minilog.conf.example all say so, and rotateIfNeeded implements exactly
    // that. Only the parser used to disagree, so the documented setting was the
    // one thing that would not load.
    for (const auto* value : {"0", "0B", "0MB"})
    {
        TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nmax_size=" + std::string(value) + "\n");
        Config cfg;
        BOOST_REQUIRE_NO_THROW(cfg = loadConfig(tmp.path));
        BOOST_TEST(cfg.outputs[0].maxSize == 0u, "value: " << value);
    }
}

BOOST_AUTO_TEST_CASE(zero_max_queue_bytes_still_throws)
{
    // The other user of the same parser. There is deliberately no "unlimited"
    // for the receive queue, so 0 has to stay an error there.
    TempFile tmp("[server]\nmax_queue_bytes=0\n\n[output.m]\ntext_file=" ABS "/tmp/f\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(size_overflow_throws_instead_of_wrapping_to_unlimited)
{
    // 2^34 GB is 2^64 bytes, which wrapped to 0 — and 0 means never rotate, so
    // asking for an enormous threshold silently asked for none at all and the
    // sink grew until the disk filled.
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nmax_size=17179869184GB\n");
    try
    {
        loadConfig(tmp.path);
        BOOST_FAIL("expected std::runtime_error for a max_size that overflows");
    }
    catch (const std::runtime_error& e)
    {
        const std::string what = e.what();
        BOOST_TEST(what.find("[output.m]") != std::string::npos, "message: " << what);
        BOOST_TEST(what.find("17179869184GB") != std::string::npos, "message: " << what);
    }
}

BOOST_AUTO_TEST_CASE(size_out_of_range_names_the_section_and_key)
{
    // stoull throws std::out_of_range, a std::logic_error — so parseOutput's
    // catch for std::runtime_error missed it and the operator got
    // "failed to load config: stoull", naming neither section nor key.
    TempFile tmp("[output.main]\ntext_file=" ABS "/tmp/f\n"
                 "max_size=99999999999999999999999999MB\n");
    try
    {
        loadConfig(tmp.path);
        BOOST_FAIL("expected std::runtime_error for an out-of-range max_size");
    }
    catch (const std::runtime_error& e)
    {
        const std::string what = e.what();
        BOOST_TEST(what.find("[output.main]") != std::string::npos, "message: " << what);
        BOOST_TEST(what.find("stoull") == std::string::npos, "message: " << what);
    }
}

BOOST_AUTO_TEST_CASE(no_numeric_part_throws)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nmax_size=MB\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(empty_size_value_no_rotation)
{
    // An empty max_size value (key present but no value) is treated as "not set"
    // by Boost PropertyTree, so maxSize stays at the default (0 = unlimited).
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nmax_size=\n");
    Config cfg = loadConfig(tmp.path);
    BOOST_TEST(cfg.outputs[0].maxSize == 0u);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Facility parsing ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(facility_parsing)

BOOST_AUTO_TEST_CASE(wildcard_gives_empty_vector)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nfacility=*\n");
    BOOST_TEST(loadConfig(tmp.path).outputs[0].facilities.empty());
}

BOOST_AUTO_TEST_CASE(missing_facility_defaults_to_wildcard)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n");
    BOOST_TEST(loadConfig(tmp.path).outputs[0].facilities.empty());
}

BOOST_AUTO_TEST_CASE(all_named_facilities)
{
    // One per canonical numeric code (0–23)
    const std::pair<std::string, int> cases[] = {
        {"kern", 0},      {"user", 1},    {"mail", 2},    {"daemon", 3},  {"auth", 4},
        {"syslog", 5},    {"lpr", 6},     {"news", 7},    {"uucp", 8},    {"clock", 9},
        {"authpriv", 10}, {"ftp", 11},    {"ntp", 12},    {"audit", 13},  {"alert", 14},
        {"clock2", 15},   {"local0", 16}, {"local1", 17}, {"local2", 18}, {"local3", 19},
        {"local4", 20},   {"local5", 21}, {"local6", 22}, {"local7", 23},
    };
    for (auto& [name, code] : cases)
    {
        TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nfacility=" + name + "\n");
        const auto facs = loadConfig(tmp.path).outputs[0].facilities;
        BOOST_REQUIRE_MESSAGE(facs.size() == 1, "facility=" + name);
        BOOST_TEST_MESSAGE("facility=" + name + " → " + std::to_string(facs[0]));
        BOOST_TEST(facs[0] == code);
    }
}

BOOST_AUTO_TEST_CASE(aliases_resolve_to_same_code)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nfacility=kernel\n");
    const auto facs = loadConfig(tmp.path).outputs[0].facilities;
    BOOST_REQUIRE(facs.size() == 1);
    BOOST_TEST(facs[0] == 0); // kernel == kern
}

BOOST_AUTO_TEST_CASE(mixed_case_facility)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nfacility=AUTH\n");
    const auto facs = loadConfig(tmp.path).outputs[0].facilities;
    BOOST_REQUIRE(facs.size() == 1);
    BOOST_TEST(facs[0] == 4);
}

BOOST_AUTO_TEST_CASE(multiple_facilities_no_duplicates)
{
    // auth and security are both code 4; should appear only once
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nfacility=auth,security\n");
    const auto facs = loadConfig(tmp.path).outputs[0].facilities;
    BOOST_TEST(facs.size() == 1u);
    BOOST_TEST(facs[0] == 4);
}

BOOST_AUTO_TEST_CASE(unknown_facility_throws)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nfacility=bogus\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(wildcard_mixed_with_names_gives_empty)
{
    // "auth,*" — the wildcard should short-circuit and return empty (= all).
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nfacility=auth,*\n");
    BOOST_TEST(loadConfig(tmp.path).outputs[0].facilities.empty());
}

BOOST_AUTO_TEST_CASE(whitespace_around_facility_names)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nfacility= auth , mail \n");
    const auto facs = loadConfig(tmp.path).outputs[0].facilities;
    BOOST_REQUIRE(facs.size() == 2);
    BOOST_TEST(facs[0] == 4); // auth
    BOOST_TEST(facs[1] == 2); // mail
}

BOOST_AUTO_TEST_CASE(empty_facility_value_defaults_to_wildcard)
{
    // "facility=" with no value → empty string → parseFacilities returns {} (wildcard).
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nfacility=\n");
    BOOST_TEST(loadConfig(tmp.path).outputs[0].facilities.empty());
}

// exclude_facility (#44): "everything except local3" without listing the other
// 23 names — which would also, as a non-wildcard list, have dropped every
// datagram that has no facility.
BOOST_AUTO_TEST_CASE(exclude_facility_absent_excludes_nothing)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n");
    BOOST_TEST(loadConfig(tmp.path).outputs[0].excludedFacilities.empty());
}

BOOST_AUTO_TEST_CASE(exclude_facility_parses_names_and_aliases)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n"
                 "exclude_facility = local3, KERNEL, local3\n");
    const auto out = loadConfig(tmp.path).outputs[0];
    BOOST_TEST(out.facilities.empty());          // facility itself is still the wildcard
    BOOST_REQUIRE(out.excludedFacilities.size() == 2);
    BOOST_TEST(out.excludedFacilities[0] == 19); // local3
    BOOST_TEST(out.excludedFacilities[1] == 0);  // kernel = kern
}

BOOST_AUTO_TEST_CASE(exclude_facility_alongside_a_named_list_is_accepted)
{
    // "auth, but not local3" excludes nothing auth would have matched. Settled
    // in #44 as a no-op rather than an error: it is harmless, and it is what
    // a generated config produces when the two keys come from different
    // templates.
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n"
                 "facility = auth\nexclude_facility = local3\n");
    const auto out = loadConfig(tmp.path).outputs[0];
    BOOST_TEST(out.facilities == std::vector<int>{4});
    BOOST_TEST(out.excludedFacilities == std::vector<int>{19});
}

BOOST_AUTO_TEST_CASE(exclude_facility_rejects_the_wildcard)
{
    // Excluding everything is a sink that can never match: refused, naming the key.
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nexclude_facility = *\n");
    BOOST_CHECK_EXCEPTION(
        loadConfig(tmp.path),
        std::runtime_error,
        [](const std::runtime_error& e)
        { return std::string(e.what()).find("[output.m] exclude_facility") != std::string::npos; });
}

BOOST_AUTO_TEST_CASE(exclude_facility_rejects_an_unknown_name)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nexclude_facility = local9\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Output section validation ────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(output_validation)

BOOST_AUTO_TEST_CASE(neither_text_nor_jsonl_throws)
{
    TempFile tmp("[output.m]\nmax_size=10MB\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(only_text_file_ok)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n");
    BOOST_CHECK_NO_THROW(loadConfig(tmp.path));
}

BOOST_AUTO_TEST_CASE(only_jsonl_file_ok)
{
    TempFile tmp("[output.m]\njsonl_file=" ABS "/tmp/f.jsonl\n");
    BOOST_CHECK_NO_THROW(loadConfig(tmp.path));
}

BOOST_AUTO_TEST_CASE(max_files_zero_means_unlimited)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nmax_files=0\n");
    Config cfg = loadConfig(tmp.path);
    BOOST_TEST(cfg.outputs[0].maxFiles == 0);
}

BOOST_AUTO_TEST_CASE(max_files_one)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nmax_files=1\n");
    Config cfg = loadConfig(tmp.path);
    BOOST_TEST(cfg.outputs[0].maxFiles == 1);
}

BOOST_AUTO_TEST_CASE(duplicate_output_section_names_throws)
{
    TempFile tmp("[output.main]\ntext_file=" ABS "/tmp/f1\n\n[output.main]\ntext_file=" ABS
                 "/tmp/f2\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(max_files_above_cap_throws)
{
    // Every generation costs a filesystem existence check, on each rotation in
    // the server and on each HTTP request in the viewer, so an unbounded value
    // is unbounded work in two places.
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nmax_files=2000000000\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);

    TempFile over("[output.m]\ntext_file=" ABS "/tmp/f\nmax_files=" +
                  std::to_string(kMaxFilesLimit + 1) + "\n");
    BOOST_CHECK_THROW(loadConfig(over.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(max_files_at_cap_accepted)
{
    TempFile tmp(
        "[output.m]\ntext_file=" ABS "/tmp/f\nmax_files=" + std::to_string(kMaxFilesLimit) + "\n");
    BOOST_TEST(loadConfig(tmp.path).outputs[0].maxFiles == kMaxFilesLimit);
}

BOOST_AUTO_TEST_CASE(max_files_negative_throws)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nmax_files=-1\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(workers_above_cap_throws)
{
    // A std::thread per worker: "workers = 1000000" used to loop until thread
    // creation failed and the std::system_error escaped to std::terminate — a
    // core dump, and on Windows an empty Event Log, for a typo.
    TempFile tmp("[server]\nworkers=257\n\n[output.m]\ntext_file=" ABS "/tmp/f\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);

    TempFile huge("[server]\nworkers=1000000\n\n[output.m]\ntext_file=" ABS "/tmp/f\n");
    BOOST_CHECK_THROW(loadConfig(huge.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(workers_at_cap_accepted)
{
    TempFile tmp("[server]\nworkers=256\n\n[output.m]\ntext_file=" ABS "/tmp/f\n");
    BOOST_TEST(loadConfig(tmp.path).workers == 256);
}

BOOST_AUTO_TEST_CASE(workers_zero_throws)
{
    TempFile tmp("[server]\nworkers=0\n\n[output.m]\ntext_file=" ABS "/tmp/f\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(workers_negative_throws)
{
    TempFile tmp("[server]\nworkers=-2\n\n[output.m]\ntext_file=" ABS "/tmp/f\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── max_queue_bytes ──────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(max_queue_bytes)

BOOST_AUTO_TEST_CASE(defaults_to_16mb)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n");
    Config cfg = loadConfig(tmp.path);
    BOOST_TEST(cfg.maxQueueBytes == 16ULL * 1024 * 1024);
}

BOOST_AUTO_TEST_CASE(accepts_size_units)
{
    TempFile tmp("[server]\nmax_queue_bytes=32MB\n\n[output.m]\ntext_file=" ABS "/tmp/f\n");
    Config cfg = loadConfig(tmp.path);
    BOOST_TEST(cfg.maxQueueBytes == 32ULL * 1024 * 1024);
}

BOOST_AUTO_TEST_CASE(accepts_plain_byte_count)
{
    TempFile tmp("[server]\nmax_queue_bytes=1048576\n\n[output.m]\ntext_file=" ABS "/tmp/f\n");
    Config cfg = loadConfig(tmp.path);
    BOOST_TEST(cfg.maxQueueBytes == 1048576u);
}

BOOST_AUTO_TEST_CASE(zero_throws_naming_the_key)
{
    // There is deliberately no way to switch admission control off — an
    // unbounded receive queue is the defect it exists to fix.
    TempFile tmp("[server]\nmax_queue_bytes=0\n\n[output.m]\ntext_file=" ABS "/tmp/f\n");
    try
    {
        loadConfig(tmp.path);
        BOOST_FAIL("expected std::runtime_error for max_queue_bytes = 0");
    }
    catch (const std::runtime_error& e)
    {
        const std::string what = e.what();
        BOOST_TEST(what.find("max_queue_bytes") != std::string::npos, "message: " << what);
    }
}

BOOST_AUTO_TEST_CASE(unknown_unit_throws_naming_the_key)
{
    TempFile tmp("[server]\nmax_queue_bytes=5PB\n\n[output.m]\ntext_file=" ABS "/tmp/f\n");
    try
    {
        loadConfig(tmp.path);
        BOOST_FAIL("expected std::runtime_error for an unknown size unit");
    }
    catch (const std::runtime_error& e)
    {
        const std::string what = e.what();
        BOOST_TEST(what.find("max_queue_bytes") != std::string::npos, "message: " << what);
    }
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Output file uniqueness ───────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(output_file_uniqueness)

BOOST_AUTO_TEST_CASE(same_path_for_text_and_jsonl_throws_naming_section)
{
    TempFile tmp("[output.main]\n"
                 "text_file=" ABS "/tmp/syslog.jsonl\n"
                 "jsonl_file=" ABS "/tmp/syslog.jsonl\n");
    try
    {
        loadConfig(tmp.path);
        BOOST_FAIL("expected std::runtime_error when both files name one path");
    }
    catch (const std::runtime_error& e)
    {
        const std::string what = e.what();
        BOOST_TEST(what.find("[output.main]") != std::string::npos, "message: " << what);
        BOOST_TEST(what.find("text_file") != std::string::npos, "message: " << what);
        BOOST_TEST(what.find("jsonl_file") != std::string::npos, "message: " << what);
        BOOST_TEST(what.find(ABS "/tmp/syslog.jsonl") != std::string::npos, "message: " << what);
    }
}

BOOST_AUTO_TEST_CASE(same_path_in_two_sections_throws_naming_both)
{
    TempFile tmp("[output.first]\n"
                 "text_file=" ABS "/tmp/shared.log\n"
                 "\n"
                 "[output.second]\n"
                 "jsonl_file=" ABS "/tmp/shared.log\n");
    try
    {
        loadConfig(tmp.path);
        BOOST_FAIL("expected std::runtime_error when two sections name one path");
    }
    catch (const std::runtime_error& e)
    {
        const std::string what = e.what();
        BOOST_TEST(what.find("[output.first]") != std::string::npos, "message: " << what);
        BOOST_TEST(what.find("[output.second]") != std::string::npos, "message: " << what);
        BOOST_TEST(what.find(ABS "/tmp/shared.log") != std::string::npos, "message: " << what);
    }
}

BOOST_AUTO_TEST_CASE(same_text_file_in_two_sections_throws)
{
    TempFile tmp("[output.a]\ntext_file=" ABS "/tmp/a.log\n\n[output.b]\ntext_file=" ABS
                 "/tmp/a.log\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(same_jsonl_file_in_two_sections_throws)
{
    TempFile tmp("[output.a]\njsonl_file=" ABS "/tmp/a.jsonl\n\n[output.b]\njsonl_file=" ABS
                 "/tmp/a.jsonl\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(distinct_paths_in_one_section_ok)
{
    TempFile tmp("[output.main]\ntext_file=" ABS "/tmp/syslog.log\njsonl_file=" ABS
                 "/tmp/syslog.jsonl\n");
    Config cfg = loadConfig(tmp.path);
    BOOST_TEST(cfg.outputs.size() == 1u);
    BOOST_TEST(cfg.outputs[0].textFile == ABS "/tmp/syslog.log");
    BOOST_TEST(cfg.outputs[0].jsonlFile == ABS "/tmp/syslog.jsonl");
}

BOOST_AUTO_TEST_CASE(distinct_paths_across_sections_ok)
{
    TempFile tmp("[output.a]\ntext_file=" ABS "/tmp/a.log\njsonl_file=" ABS "/tmp/a.jsonl\n"
                 "\n"
                 "[output.b]\ntext_file=" ABS "/tmp/b.log\njsonl_file=" ABS "/tmp/b.jsonl\n");
    Config cfg = loadConfig(tmp.path);
    BOOST_TEST(cfg.outputs.size() == 2u);
}

BOOST_AUTO_TEST_CASE(sections_configuring_only_one_kind_do_not_collide)
{
    // Both sections leave one of the two keys empty. The empty string is not a
    // path and must not count as a collision.
    TempFile tmp("[output.a]\ntext_file=" ABS "/tmp/a.log\n\n[output.b]\njsonl_file=" ABS
                 "/tmp/b.jsonl\n");
    Config cfg = loadConfig(tmp.path);
    BOOST_TEST(cfg.outputs.size() == 2u);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Unknown keys ─────────────────────────────────────────────────────────────
//
// A key minilog does not understand is a config error. It used to be accepted
// and ignored, so "max_sise = 100MB" left the size at its default and
// "enabeld = true" left forwarding off, with a running server doing something
// other than what the file said and nothing anywhere to say so.

BOOST_AUTO_TEST_SUITE(unknown_keys)

BOOST_AUTO_TEST_CASE(unknown_server_key_throws_naming_section_and_key)
{
    TempFile tmp("[server]\nudp_prot = 514\n\n[output.m]\ntext_file=" ABS "/tmp/f\n");
    try
    {
        loadConfig(tmp.path);
        BOOST_FAIL("expected std::runtime_error for an unknown [server] key");
    }
    catch (const std::runtime_error& e)
    {
        const std::string what = e.what();
        BOOST_TEST(what.find("udp_prot") != std::string::npos, "message: " << what);
        BOOST_TEST(what.find("[server]") != std::string::npos, "message: " << what);
        // The valid keys are listed, because the point of failing is to be fixable.
        BOOST_TEST(what.find("udp_port") != std::string::npos, "message: " << what);
    }
}

BOOST_AUTO_TEST_CASE(unknown_output_key_throws_naming_the_section)
{
    TempFile tmp("[output.main]\ntext_file=" ABS "/tmp/f\nmax_sise=100MB\n");
    try
    {
        loadConfig(tmp.path);
        BOOST_FAIL("expected std::runtime_error for an unknown [output.*] key");
    }
    catch (const std::runtime_error& e)
    {
        const std::string what = e.what();
        BOOST_TEST(what.find("max_sise") != std::string::npos, "message: " << what);
        BOOST_TEST(what.find("[output.main]") != std::string::npos, "message: " << what);
    }
}

BOOST_AUTO_TEST_CASE(unknown_forwarding_key_throws)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n"
                 "[forwarding]\nenabeld=true\nhost=10.0.0.5\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(unknown_web_viewer_key_throws)
{
    // The section belongs to the web viewer, but this is the only component
    // that validates the file, so a typo here has to fail here or nowhere.
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n\n[web_viewer]\nprot=8080\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(web_viewer_section_accepted)
{
    // minilog ignores what the section means and must keep starting with it
    // present — the installer ships a config that has one.
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n\n[web_viewer]\nhost=\nport=9514\n");
    BOOST_CHECK_NO_THROW(loadConfig(tmp.path));
}

BOOST_AUTO_TEST_CASE(unknown_section_is_ignored)
{
    // Only keys in sections minilog owns are checked. The file is shared with
    // other components, and claiming the whole section namespace would mean the
    // server rejecting a config the next tool to read it needs — which is
    // exactly what would have happened to [web_viewer].
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n\n[something_else]\nkey=value\n");
    BOOST_CHECK_NO_THROW(loadConfig(tmp.path));
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Absolute path requirement ────────────────────────────────────────────────
//
// A relative log path resolves against the working directory of whichever
// process reads the config, and the server, cli-viewer and web-viewer each have
// a different one. Rejecting it at load is what lets the viewers use the
// configured value as written.

BOOST_AUTO_TEST_SUITE(absolute_paths)

BOOST_AUTO_TEST_CASE(relative_text_file_throws_naming_section_and_key)
{
    TempFile tmp("[output.main]\ntext_file=logs/syslog.log\n");
    try
    {
        loadConfig(tmp.path);
        BOOST_FAIL("expected std::runtime_error for a relative text_file");
    }
    catch (const std::runtime_error& e)
    {
        const std::string what = e.what();
        BOOST_TEST(what.find("[output.main] text_file") != std::string::npos, "message: " << what);
        BOOST_TEST(what.find("logs/syslog.log") != std::string::npos, "message: " << what);
        BOOST_TEST(what.find("absolute") != std::string::npos, "message: " << what);
        // People will try %ProgramData%, so the message has to say why it fails.
        BOOST_TEST(what.find("Environment variables are not expanded") != std::string::npos,
                   "message: " << what);
    }
}

BOOST_AUTO_TEST_CASE(relative_jsonl_file_throws_naming_section_and_key)
{
    TempFile tmp("[output.aux]\njsonl_file=syslog.jsonl\n");
    try
    {
        loadConfig(tmp.path);
        BOOST_FAIL("expected std::runtime_error for a relative jsonl_file");
    }
    catch (const std::runtime_error& e)
    {
        const std::string what = e.what();
        BOOST_TEST(what.find("[output.aux] jsonl_file") != std::string::npos, "message: " << what);
        BOOST_TEST(what.find("syslog.jsonl") != std::string::npos, "message: " << what);
    }
}

BOOST_AUTO_TEST_CASE(unexpanded_environment_variable_throws)
{
    // %ProgramData%\minilog\logs is relative as written; without this check it
    // would create a literal "%ProgramData%" directory next to the CWD.
    TempFile tmp("[output.m]\ntext_file=%ProgramData%\\minilog\\syslog.log\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(dot_relative_path_throws)
{
    TempFile tmp("[output.m]\ntext_file=./syslog.log\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(absolute_path_accepted)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/syslog.log\n");
    BOOST_CHECK_NO_THROW(loadConfig(tmp.path));
}

#ifdef _WIN32

BOOST_AUTO_TEST_CASE(unc_path_accepted)
{
    // Centralised logging onto a share is a plausible deployment, and a UNC path
    // is absolute. This test exists so the check is not later "simplified" into a
    // drive-letter test, which would reject it.
    TempFile tmp("[output.m]\njsonl_file=\\\\server\\share\\logs\\syslog.jsonl\n");
    Config cfg = loadConfig(tmp.path);
    BOOST_TEST(cfg.outputs[0].jsonlFile == "\\\\server\\share\\logs\\syslog.jsonl");
}

BOOST_AUTO_TEST_CASE(drive_relative_path_throws)
{
    // "C:syslog.log" names the current directory *of drive C:*, which is no more
    // predictable than a plain relative path.
    TempFile tmp("[output.m]\ntext_file=C:syslog.log\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(posix_rooted_path_throws)
{
    // Absolute on POSIX, drive-relative on Windows. Rejecting it here is the
    // reason the tests above carry a drive letter.
    TempFile tmp("[output.m]\ntext_file=/var/log/syslog.log\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

#endif

BOOST_AUTO_TEST_SUITE_END()

// ─── File I/O edge cases ──────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(file_io)

BOOST_AUTO_TEST_CASE(missing_file_throws)
{
    BOOST_CHECK_THROW(loadConfig("/nonexistent/path/minilog.conf"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(crlf_line_endings_parsed)
{
    TempFile tmp("[server]\r\nudp_port = 5514\r\n\r\n[output.m]\r\ntext_file=" ABS "/tmp/f\r\n");
    Config cfg = loadConfig(tmp.path);
    BOOST_TEST(cfg.udpPort == 5514);
}

BOOST_AUTO_TEST_CASE(semicolon_and_hash_are_part_of_the_value)
{
    // There are no inline comments in this file format: a ';' or '#' after the
    // '=' belongs to the value, which is what Boost's INI parser does and what
    // the server therefore defines. The web-viewer used to strip from the first
    // one and so opened "hash" while the server wrote "hash#name.log"; this test
    // pins the behaviour the viewer now matches.
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/hash#name.log\n"
                 "jsonl_file=" ABS "/tmp/semi;colon.jsonl\n");
    Config cfg = loadConfig(tmp.path);
    BOOST_TEST(cfg.outputs[0].textFile == ABS "/tmp/hash#name.log");
    BOOST_TEST(cfg.outputs[0].jsonlFile == ABS "/tmp/semi;colon.jsonl");
}

BOOST_AUTO_TEST_CASE(trailing_comment_on_max_files_is_a_config_error)
{
    // Boost's INI parser keeps the whole value, which is what makes a `#` or `;`
    // legal in a path. The cost is that an inline comment ends up in the value,
    // and `7 ; keep 7 generations` is not a number.
    //
    // It used to fall back to the default of 10, which the web viewer did too,
    // so at least the two ends agreed. Somebody who wrote that line expecting
    // the comment to be stripped is better told than quietly given a different
    // rotation depth — the same argument as for any other unparseable value.
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\nmax_files=7 ; keep 7 generations\n");
    try
    {
        loadConfig(tmp.path);
        BOOST_FAIL("expected std::runtime_error for max_files with an inline comment");
    }
    catch (const std::runtime_error& e)
    {
        const std::string what = e.what();
        BOOST_TEST(what.find("[output.m] max_files") != std::string::npos, "message: " << what);
        BOOST_TEST(what.find("7 ; keep 7 generations") != std::string::npos, "message: " << what);
    }
}

BOOST_AUTO_TEST_CASE(path_with_spaces)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/my log dir/syslog.log\n");
    Config cfg = loadConfig(tmp.path);
    BOOST_TEST(cfg.outputs[0].textFile == ABS "/tmp/my log dir/syslog.log");
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Forwarding section ───────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(server_host)

BOOST_AUTO_TEST_CASE(hostname_throws_naming_key_and_value)
{
    TempFile tmp("[server]\nhost=localhost\n\n[output.m]\ntext_file=" ABS "/tmp/f\n");
    try
    {
        loadConfig(tmp.path);
        BOOST_FAIL("expected std::runtime_error for a non-address server host");
    }
    catch (const std::runtime_error& e)
    {
        const std::string what = e.what();
        BOOST_TEST(what.find("[server] host") != std::string::npos, "message: " << what);
        BOOST_TEST(what.find("localhost") != std::string::npos, "message: " << what);
    }
}

BOOST_AUTO_TEST_CASE(malformed_address_throws)
{
    TempFile tmp("[server]\nhost=192.168.1.999\n\n[output.m]\ntext_file=" ABS "/tmp/f\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(ipv4_and_wildcard_accepted)
{
    TempFile tmp("[server]\nhost=127.0.0.1\n\n[output.m]\ntext_file=" ABS "/tmp/f\n");
    BOOST_CHECK_EQUAL(loadConfig(tmp.path).host, "127.0.0.1");

    TempFile wild("[server]\nhost=0.0.0.0\n\n[output.m]\ntext_file=" ABS "/tmp/f\n");
    BOOST_CHECK_EQUAL(loadConfig(wild.path).host, "0.0.0.0");
}

BOOST_AUTO_TEST_CASE(ipv6_host_accepted)
{
    // #24 settled that IPv6 literals keep working rather than being rejected,
    // so validation checks parseability only, never the address family.
    TempFile tmp("[server]\nhost=::1\n\n[output.m]\ntext_file=" ABS "/tmp/f\n");
    BOOST_CHECK_EQUAL(loadConfig(tmp.path).host, "::1");

    // Colons are only rejected for addresses that parse as IPv4, so an
    // IPv4-mapped IPv6 literal must still be accepted.
    TempFile mapped("[server]\nhost=::ffff:10.0.0.5\n\n[output.m]\ntext_file=" ABS "/tmp/f\n");
    BOOST_CHECK_NO_THROW(loadConfig(mapped.path));
}

BOOST_AUTO_TEST_CASE(address_with_port_throws)
{
    TempFile tmp("[server]\nhost=127.0.0.1:514\n\n[output.m]\ntext_file=" ABS "/tmp/f\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(bracketed_ipv6_with_port_throws)
{
    // The Windows parser accepts this form too and discards the port, and the
    // is_v4() colon check cannot see it — an IPv6 literal parses as v6.
    TempFile tmp("[server]\nhost=[::1]:514\n\n[output.m]\ntext_file=" ABS "/tmp/f\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(bracketed_ipv6_without_port_throws)
{
    // Brackets are never valid input to make_address on any platform.
    TempFile tmp("[server]\nhost=[::1]\n\n[output.m]\ntext_file=" ABS "/tmp/f\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(forwarding_section)

BOOST_AUTO_TEST_CASE(forwarding_port_zero_throws)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n\n[forwarding]\nport=0\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(forwarding_port_65536_throws)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n\n[forwarding]\nport=65536\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(forwarding_absent_gives_defaults)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n");
    const auto& fwd = loadConfig(tmp.path).forwarding;
    BOOST_TEST(!fwd.enabled);
    BOOST_TEST(fwd.port == 514);
    BOOST_TEST(fwd.maxMessageSize == 2048u);
    BOOST_TEST(fwd.facilities.empty());
}

BOOST_AUTO_TEST_CASE(forwarding_facility_filter)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n"
                 "[forwarding]\nfacility=local0,local1\n");
    const auto facs = loadConfig(tmp.path).forwarding.facilities;
    BOOST_REQUIRE(facs.size() == 2);
    BOOST_TEST(facs[0] == 16);
    BOOST_TEST(facs[1] == 17);
}

BOOST_AUTO_TEST_CASE(forwarding_exclude_facility)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n"
                 "[forwarding]\nexclude_facility=local3\n");
    const auto& fwd = loadConfig(tmp.path).forwarding;
    BOOST_TEST(fwd.facilities.empty());
    BOOST_TEST(fwd.excludedFacilities == std::vector<int>{19});
}

BOOST_AUTO_TEST_CASE(forwarding_exclude_facility_rejects_the_wildcard)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n"
                 "[forwarding]\nexclude_facility=*\n");
    BOOST_CHECK_EXCEPTION(loadConfig(tmp.path),
                          std::runtime_error,
                          [](const std::runtime_error& e) {
                              return std::string(e.what()).find("[forwarding] exclude_facility") !=
                                     std::string::npos;
                          });
}

// Both host fields used to reach boost::asio::make_address unvalidated, where an
// unparseable value aborted the process (forwarding) or exited silently
// (server). They are validated here now — but by different rules. [server] host
// binds an interface and must be an IP literal; [forwarding] host names a
// destination, so a hostname is exactly what a deployment usually wants, and the
// check only rules out what cannot be a host at all.

BOOST_AUTO_TEST_CASE(forwarding_hostname_accepted)
{
    // Naming the collector is the normal deployment shape. The Forwarder
    // resolves it; whether it resolves today is not a question loadConfig can
    // answer, and refusing to start over it would make a service that comes up
    // before DNS does unstartable.
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n"
                 "[forwarding]\nenabled=true\nhost=syslog.example.com\n");
    Config cfg;
    BOOST_REQUIRE_NO_THROW(cfg = loadConfig(tmp.path));
    BOOST_TEST(cfg.forwarding.host == "syslog.example.com");
}

BOOST_AUTO_TEST_CASE(forwarding_unresolvable_name_is_not_a_config_error)
{
    // .invalid is reserved so that it never resolves (RFC 2606), and it is still
    // not loadConfig's business: the failure is reported at startup and retried,
    // which is what makes DNS arriving late survivable.
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n"
                 "[forwarding]\nenabled=true\nhost=collector.invalid\n");
    BOOST_CHECK_NO_THROW(loadConfig(tmp.path));
}

BOOST_AUTO_TEST_CASE(forwarding_host_with_underscore_accepted)
{
    // Not a legal hostname by RFC 1123, but common in practice and resolvable
    // through hosts files. Nothing here has to have an opinion about it.
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n"
                 "[forwarding]\nenabled=true\nhost=log_collector\n");
    BOOST_CHECK_NO_THROW(loadConfig(tmp.path));
}

BOOST_AUTO_TEST_CASE(forwarding_host_with_scheme_throws)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n"
                 "[forwarding]\nenabled=true\nhost=udp://syslog.example.com\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(forwarding_host_with_space_throws)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n"
                 "[forwarding]\nenabled=true\nhost=syslog example com\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(forwarding_hostname_with_port_throws)
{
    // The same mistake as the address-with-port case below, but on a name, where
    // there is no parser to catch it: left alone it would be a name that never
    // resolves, reported once and then retried forever.
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n"
                 "[forwarding]\nenabled=true\nhost=syslog.example.com:514\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(forwarding_address_with_port_throws)
{
    // Accepted by the Windows address parser, which discards the port — so this
    // has to be rejected explicitly rather than left to make_address.
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n"
                 "[forwarding]\nenabled=true\nhost=10.0.0.5:514\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(forwarding_malformed_address_throws)
{
    // Digits and dots only, and not a valid address. A hostname cannot have an
    // all-numeric top-level label, so this is a mistyped address rather than a
    // name — which is what lets it be rejected loudly instead of retried forever
    // as a name that will never resolve.
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n"
                 "[forwarding]\nenabled=true\nhost=10.0.0.999\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(forwarding_host_not_validated_when_disabled)
{
    // Nothing constructs a Forwarder in this case, and rejecting it would break
    // configs that work today.
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n"
                 "[forwarding]\nenabled=false\nhost=syslog.example.com\n");
    BOOST_CHECK_NO_THROW(loadConfig(tmp.path));
}

BOOST_AUTO_TEST_CASE(forwarding_ipv6_host_accepted)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n"
                 "[forwarding]\nenabled=true\nhost=::1\n");
    BOOST_CHECK_NO_THROW(loadConfig(tmp.path));
}

BOOST_AUTO_TEST_CASE(forwarding_enabled_no_host_throws)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n"
                 "[forwarding]\nenabled=true\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(forwarding_port_non_numeric_throws)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n"
                 "[forwarding]\nport=abc\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(forwarding_port_trailing_chars_throws)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n"
                 "[forwarding]\nport=514x\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(forwarding_port_negative_throws)
{
    TempFile tmp("[output.m]\ntext_file=" ABS "/tmp/f\n"
                 "[forwarding]\nport=-1\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Output section validation ────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(output_section_validation)

BOOST_AUTO_TEST_CASE(no_output_sections_throws)
{
    TempFile tmp("[server]\nudp_port = 5514\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(empty_config_throws)
{
    TempFile tmp("");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(duplicate_output_section_name_throws)
{
    TempFile tmp("[output.main]\n"
                 "text_file = " ABS "/tmp/a.log\n"
                 "\n"
                 "[output.main]\n"
                 "text_file = " ABS "/tmp/b.log\n");
    BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

// ─── Unparseable values ──────────────────────────────────────────────────────
//
// property_tree's get<T>(path, default) returns the default on a translation
// failure as well as on an absent key, so an unparseable value was accepted
// silently while an unknown *key* was a hard error — the two halves of the same
// guarantee disagreeing. A misspelled key and a misspelled value are the same
// operator mistake with the same consequence.

BOOST_AUTO_TEST_SUITE(unparseable_values)

namespace
{

// Assert that loading fails and that the message names the section and key
// (together, as they appear in the file) and the value that could not be read.
// Naming all three is the point: the operator has to be able to find the line.
void expectRejected(const std::string& content,
                    const std::string& sectionAndKey,
                    const std::string& value)
{
    TempFile tmp(content);
    try
    {
        loadConfig(tmp.path);
        BOOST_FAIL("expected std::runtime_error for " + sectionAndKey + " = " + value);
    }
    catch (const std::runtime_error& e)
    {
        const std::string what = e.what();
        BOOST_TEST(what.find(sectionAndKey) != std::string::npos, "message: " << what);
        BOOST_TEST(what.find(value) != std::string::npos, "message: " << what);
    }
}

const std::string kOutputPrefix = "[output.main]\ntext_file=" ABS "/tmp/f\n";
const std::string kForwardPrefix =
    "[output.main]\ntext_file=" ABS "/tmp/f\n\n[forwarding]\nhost = 127.0.0.1\n";

} // namespace

BOOST_AUTO_TEST_CASE(max_files_not_a_number_is_rejected)
{
    // Silently 10 before this, while "max_sise = 100MB" — a typo one character
    // away — was already a startup failure naming the section and the key.
    expectRejected(kOutputPrefix + "max_files = abc\n", "[output.main] max_files", "abc");
}

BOOST_AUTO_TEST_CASE(include_malformed_not_a_bool_is_rejected)
{
    // Silently true before this, so a sink asked to drop malformed datagrams
    // kept writing them.
    expectRejected(
        kOutputPrefix + "include_malformed = yess\n", "[output.main] include_malformed", "yess");
}

BOOST_AUTO_TEST_CASE(max_message_size_not_a_number_is_rejected)
{
    // "2k" is the mistake worth expecting here: max_size takes a unit suffix
    // and max_message_size does not.
    expectRejected(
        kForwardPrefix + "max_message_size = 2k\n", "[forwarding] max_message_size", "2k");
}

BOOST_AUTO_TEST_CASE(negative_max_message_size_is_rejected)
{
    // std::stoul accepts a leading '-' and wraps, so this would otherwise have
    // become 4294967295 — a truncation limit of four gigabytes, which is no
    // limit at all.
    expectRejected(
        kForwardPrefix + "max_message_size = -1\n", "[forwarding] max_message_size", "-1");
}

BOOST_AUTO_TEST_CASE(forwarding_enabled_not_a_bool_is_rejected)
{
    // Not in the original report, and the worst of the four: the default is
    // false, so a misspelt value turned forwarding off and said nothing. The
    // section is otherwise complete and correct.
    expectRejected(kForwardPrefix + "enabled = yess\n", "[forwarding] enabled", "yess");
}

BOOST_AUTO_TEST_CASE(the_accepted_bool_spellings_are_exactly_boosts)
{
    // true / false / 1 / 0, which is the set property_tree's own translator
    // took. yes/no/on/off would be a larger promise than this fix needs, and
    // the documentation would have to carry it.
    for (const auto* yes : {"true", "1"})
    {
        TempFile tmp(kOutputPrefix + "include_malformed = " + yes + "\n");
        BOOST_TEST(loadConfig(tmp.path).outputs[0].includeMalformed, "spelling: " << yes);
    }
    for (const auto* no : {"false", "0"})
    {
        TempFile tmp(kOutputPrefix + "include_malformed = " + no + "\n");
        BOOST_TEST(!loadConfig(tmp.path).outputs[0].includeMalformed, "spelling: " << no);
    }

    for (const auto* spelled : {"TRUE", "True", "yes", "on"})
    {
        TempFile tmp(kOutputPrefix + "include_malformed = " + spelled + "\n");
        BOOST_CHECK_THROW(loadConfig(tmp.path), std::runtime_error);
    }
}

BOOST_AUTO_TEST_CASE(forwarding_enabled_accepts_the_same_spellings)
{
    TempFile on(kForwardPrefix + "enabled = 1\n");
    BOOST_TEST(loadConfig(on.path).forwarding.enabled);

    TempFile off(kForwardPrefix + "enabled = false\n");
    BOOST_TEST(!loadConfig(off.path).forwarding.enabled);
}

BOOST_AUTO_TEST_CASE(a_valid_value_still_loads)
{
    // The check must not have turned a working config into an error: every key
    // this touches, set to something ordinary.
    TempFile tmp(kOutputPrefix + "max_files = 3\ninclude_malformed = false\n" +
                 "\n[forwarding]\nenabled = true\nhost = 127.0.0.1\nport = 5514\n"
                 "max_message_size = 4096\n");
    Config cfg = loadConfig(tmp.path);

    BOOST_TEST(cfg.outputs[0].maxFiles == 3);
    BOOST_TEST(!cfg.outputs[0].includeMalformed);
    BOOST_TEST(cfg.forwarding.enabled);
    BOOST_TEST(cfg.forwarding.maxMessageSize == 4096u);
}

BOOST_AUTO_TEST_CASE(integer_errors_name_the_section_too)
{
    // The three keys that already rejected non-integers named only the key —
    // "Invalid integer value for 'port'" left the operator to find which
    // section it came from. They go through the same reader now.
    expectRejected("[server]\nudp_port = abc\n\n" + kOutputPrefix, "[server] udp_port", "abc");
    expectRejected("[server]\nworkers = 4x\n\n" + kOutputPrefix, "[server] workers", "4x");
    expectRejected(kForwardPrefix + "port = 514x\n", "[forwarding] port", "514x");
}

BOOST_AUTO_TEST_SUITE_END()
