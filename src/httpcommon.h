/**
 * @file src/httpcommon.h
 * @brief Declarations for common HTTP.
 */
#pragma once

// standard includes
#include <set>
#include <string>

// lib includes
#include <curl/curl.h>

// local includes
#include "network.h"
#include "thread_safe.h"
#include "uuid.h"

namespace http {

  int init();
  int create_creds(const std::string &pkey, const std::string &cert);
  int save_user_creds(
    const std::string &file,
    const std::string &username,
    const std::string &password,
    bool run_our_mouth = false
  );

  int reload_user_creds(const std::string &file);
  bool download_file(const std::string &url, const std::string &file, long ssl_version = CURL_SSLVERSION_TLSv1_2);
  std::string url_escape(const std::string &url);
  std::string url_get_host(const std::string &url);

  /**
   * @brief Hash an API token for storage/comparison (unsalted SHA-256, hex).
   * @param token The plaintext token.
   * @return Lowercase hex SHA-256 of the token.
   */
  std::string hash_api_token(const std::string &token);

  /**
   * @brief Extract a Bearer token from an Authorization header value.
   * @param authorization_header The raw header value (e.g. "Bearer abc123").
   * @return The token, or "" if the header is absent/malformed/not Bearer.
   */
  std::string extract_bearer_token(const std::string &authorization_header);

  /**
   * @brief Persist (or clear) the API token hash in the credentials file.
   * @param file The credentials file path.
   * @param token_hash The hash to store; pass "" to remove it.
   * @return 0 on success, -1 on error.
   */
  int save_api_token(const std::string &file, const std::string &token_hash);

  /**
   * @brief Decide whether a request may be authorized by the read-only API key.
   *
   * The key is read-only and least-privilege: it is honored only on GET requests to an
   * allowlisted path, only when a key is configured, and only when the presented Bearer
   * token hashes to the stored hash. This is the pure gating decision used by the
   * config server's authenticate(); origin gating is bypassed by the caller because the
   * key is itself the credential.
   *
   * @param method The HTTP method (e.g. "GET").
   * @param path The request path (e.g. "/api/clients/list").
   * @param authorization_header The raw Authorization header value.
   * @param configured_token_hash The stored token hash; empty disables the key.
   * @param allowed_paths The set of paths the key may reach.
   * @return True if the request is authorized by the API key.
   */
  bool is_api_key_authorized(
    const std::string &method,
    const std::string &path,
    const std::string &authorization_header,
    const std::string &configured_token_hash,
    const std::set<std::string> &allowed_paths
  );

  extern std::string unique_id;
  extern uuid_util::uuid_t uuid;
  extern net::net_e origin_web_ui_allowed;

}  // namespace http
