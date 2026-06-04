/**
 * @file src/httpcommon.h
 * @brief Declarations for common HTTP.
 */
#pragma once

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

  extern std::string unique_id;
  extern uuid_util::uuid_t uuid;
  extern net::net_e origin_web_ui_allowed;

}  // namespace http
