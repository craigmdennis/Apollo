/**
 * @file tests/unit/test_httpcommon.cpp
 * @brief Test src/httpcommon.*.
 */
// test imports
#include "../tests_common.h"

// standard imports
#include <filesystem>
#include <fstream>
#include <set>

// lib imports
#include <curl/curl.h>

// local imports
#include <src/config.h>
#include <src/httpcommon.h>

struct UrlEscapeTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(UrlEscapeTest, Run) {
  const auto &[input, expected] = GetParam();
  ASSERT_EQ(http::url_escape(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  UrlEscapeTests,
  UrlEscapeTest,
  testing::Values(
    std::make_tuple("igdb_0123456789", "igdb_0123456789"),
    std::make_tuple("../../../", "..%2F..%2F..%2F"),
    std::make_tuple("..*\\", "..%2A%5C")
  )
);

struct UrlGetHostTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(UrlGetHostTest, Run) {
  const auto &[input, expected] = GetParam();
  ASSERT_EQ(http::url_get_host(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  UrlGetHostTests,
  UrlGetHostTest,
  testing::Values(
    std::make_tuple("https://images.igdb.com/example.txt", "images.igdb.com"),
    std::make_tuple("http://localhost:8080", "localhost"),
    std::make_tuple("nonsense!!}{::", "")
  )
);

struct DownloadFileTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(DownloadFileTest, Run) {
  const auto &[url, filename] = GetParam();
  const std::string test_dir = platf::appdata().string() + "/tests/";
  std::string path = test_dir + filename;
  ASSERT_TRUE(http::download_file(url, path, CURL_SSLVERSION_TLSv1_0));
}

#ifdef SUNSHINE_BUILD_FLATPAK
// requires running `npm run serve` prior to running the tests
constexpr const char *URL_1 = "http://0.0.0.0:3000/hello.txt";
constexpr const char *URL_2 = "http://0.0.0.0:3000/hello-redirect.txt";
#else
constexpr const char *URL_1 = "https://httpbin.org/base64/aGVsbG8h";
constexpr const char *URL_2 = "https://httpbin.org/redirect-to?url=/base64/aGVsbG8h";
#endif

INSTANTIATE_TEST_SUITE_P(
  DownloadFileTests,
  DownloadFileTest,
  testing::Values(
    std::make_tuple(URL_1, "hello.txt"),
    std::make_tuple(URL_2, "hello-redirect.txt")
  )
);

// --- hash_api_token ---
TEST(HashApiToken, IsDeterministic) {
  ASSERT_EQ(http::hash_api_token("my-secret-key"), http::hash_api_token("my-secret-key"));
}

TEST(HashApiToken, DiffersByInput) {
  ASSERT_NE(http::hash_api_token("key-a"), http::hash_api_token("key-b"));
}

TEST(HashApiToken, IsSha256HexLength) {
  // SHA-256 = 32 bytes = 64 hex chars
  ASSERT_EQ(http::hash_api_token("anything").size(), 64u);
}

// --- extract_bearer_token ---
struct ExtractBearerTokenTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(ExtractBearerTokenTest, Run) {
  const auto &[header, expected] = GetParam();
  ASSERT_EQ(http::extract_bearer_token(header), expected);
}

INSTANTIATE_TEST_SUITE_P(
  ExtractBearerTokenTests,
  ExtractBearerTokenTest,
  testing::Values(
    std::make_tuple("Bearer abc123", "abc123"),
    std::make_tuple("bearer abc123", "abc123"),      // scheme is case-insensitive
    std::make_tuple("BEARER abc123", "abc123"),
    std::make_tuple("Bearer   abc123  ", "abc123"),  // surrounding whitespace trimmed
    std::make_tuple("Basic abc123", ""),             // wrong scheme
    std::make_tuple("Bearer ", ""),                  // empty token
    std::make_tuple("Bearertoken", ""),              // no separator
    std::make_tuple("", "")
  )
);

// --- API token persistence ---
TEST(ApiTokenPersistence, SaveLoadRoundTrip) {
  const std::string file = (std::filesystem::temp_directory_path() / "apollo_creds_test.json").string();
  std::filesystem::remove(file);
  // Seed a minimal creds file so save merges into existing JSON.
  { std::ofstream o(file); o << R"({"username":"u","salt":"s","password":"p"})"; }

  const std::string hash = http::hash_api_token("the-key");
  ASSERT_EQ(http::save_api_token(file, hash), 0);

  config::sunshine.api_token = "stale";
  ASSERT_EQ(http::reload_user_creds(file), 0);
  ASSERT_EQ(config::sunshine.api_token, hash);

  // Clearing removes it; reload yields empty.
  ASSERT_EQ(http::save_api_token(file, ""), 0);
  ASSERT_EQ(http::reload_user_creds(file), 0);
  ASSERT_EQ(config::sunshine.api_token, "");

  std::filesystem::remove(file);
}

// --- is_api_key_authorized (the read-only gating decision) ---
TEST(IsApiKeyAuthorized, GatingContract) {
  const std::set<std::string> allowed {"/api/clients/list"};
  const std::string hash = http::hash_api_token("good-key");
  const std::string good = "Bearer good-key";

  // Authorized: configured key, GET, allowlisted path, matching token.
  EXPECT_TRUE(http::is_api_key_authorized("GET", "/api/clients/list", good, hash, allowed));

  // Disabled when no key is configured.
  EXPECT_FALSE(http::is_api_key_authorized("GET", "/api/clients/list", good, "", allowed));

  // Read-only: a non-GET method is never authorized, even with a valid key.
  EXPECT_FALSE(http::is_api_key_authorized("POST", "/api/clients/list", good, hash, allowed));
  EXPECT_FALSE(http::is_api_key_authorized("DELETE", "/api/clients/list", good, hash, allowed));

  // Scope: a path that is not allowlisted is rejected (e.g. config/logs).
  EXPECT_FALSE(http::is_api_key_authorized("GET", "/api/config", good, hash, allowed));

  // Wrong key rejected.
  EXPECT_FALSE(http::is_api_key_authorized("GET", "/api/clients/list", "Bearer wrong", hash, allowed));

  // No Bearer credential rejected.
  EXPECT_FALSE(http::is_api_key_authorized("GET", "/api/clients/list", "", hash, allowed));
  EXPECT_FALSE(http::is_api_key_authorized("GET", "/api/clients/list", "Basic good-key", hash, allowed));
}
