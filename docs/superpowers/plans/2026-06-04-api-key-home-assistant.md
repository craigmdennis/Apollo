# Read-only API Key for Headless Integrations — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a headless client (Home Assistant) authenticate to Apollo's config API with a read-only API key, so it can poll `GET /api/clients/list` for which device is connected — without a browser session and without the origin 403.

**Architecture:** Add a stateless Bearer-token path to the existing `confighttp::authenticate()` gate. The key is honored only on `GET` requests to an allowlisted set of safe paths (initially `/api/clients/list`), making it structurally read-only and least-privilege. The key is generated/revoked from the web UI, stored as an unsalted SHA-256 hash in the existing credentials file. No new data endpoint — it reuses the existing `connected` data.

**Tech Stack:** C++17, CMake/Ninja, GoogleTest, Boost, nlohmann::json, Simple-Web-Server (HTTPS), Vue 3.

**Spec:** `docs/superpowers/specs/2026-06-04-api-key-home-assistant-design.md`

---

## File Structure

- `src/config.h` — add `api_token` (the stored hash) to `sunshine_t`.
- `src/httpcommon.h` / `src/httpcommon.cpp` — pure helpers `hash_api_token()`, `extract_bearer_token()`; persistence `save_api_token()`; load `api_token` in `reload_user_creds()`.
- `src/confighttp.cpp` — Bearer path in `authenticate()`; `TOKEN_ALLOWED_PATHS`; `generateApiToken`/`revokeApiToken` handlers; `/api/token` routes.
- `tests/unit/test_httpcommon.cpp` — unit tests for the pure helpers.
- `src_assets/common/assets/web/password.html` — "API Access" UI section.
- `src_assets/common/assets/web/public/assets/locale/en.json` — i18n strings.
- `docs/api.md` — document `/api/token` + Home Assistant usage.

**Why helpers live in `httpcommon`:** the HTTPS-coupled `authenticate()` can't be unit-tested directly, so the hashable/parsable logic is extracted into pure functions that can. `authenticate()` just composes them.

---

## Task 1: Pure helpers — `hash_api_token` + `extract_bearer_token`

**Files:**
- Modify: `src/httpcommon.h` (add declarations)
- Modify: `src/httpcommon.cpp` (add definitions)
- Test: `tests/unit/test_httpcommon.cpp`

- [ ] **Step 1: Write the failing tests**

Add to the end of `tests/unit/test_httpcommon.cpp`:

```cpp
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
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake -B build -G Ninja -S . -DBUILD_TESTS=ON && ninja -C build`
Expected: **compile failure** — `http::hash_api_token` / `http::extract_bearer_token` are not declared.

- [ ] **Step 3: Declare the helpers**

In `src/httpcommon.h`, inside `namespace http { ... }` (next to the other free-function declarations such as `reload_user_creds`), add:

```cpp
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
```

- [ ] **Step 4: Implement the helpers**

In `src/httpcommon.cpp`, ensure these includes are present near the top (add any missing):

```cpp
#include <boost/algorithm/string.hpp>
#include "crypto.h"
#include "utility.h"
```

Add the definitions inside `namespace http`:

```cpp
  std::string hash_api_token(const std::string &token) {
    return util::hex(crypto::hash(token)).to_string();
  }

  std::string extract_bearer_token(const std::string &authorization_header) {
    using namespace std::literals;
    constexpr auto prefix = "bearer "sv;
    if (authorization_header.size() <= prefix.size()) {
      return "";
    }
    std::string scheme = authorization_header.substr(0, prefix.size());
    boost::algorithm::to_lower(scheme);
    if (scheme != prefix) {
      return "";
    }
    std::string token = authorization_header.substr(prefix.size());
    boost::algorithm::trim(token);
    return token;
  }
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `ninja -C build && ./build/tests/test_sunshine --gtest_filter='HashApiToken*:ExtractBearerTokenTests*'`
Expected: PASS (all cases).

- [ ] **Step 6: Commit**

```bash
git add src/httpcommon.h src/httpcommon.cpp tests/unit/test_httpcommon.cpp
git commit -m "feat(api): add hash_api_token + extract_bearer_token helpers"
```

---

## Task 2: Config field + credential persistence

**Files:**
- Modify: `src/config.h:271` (add field after `salt`)
- Modify: `src/httpcommon.h` (declare `save_api_token`)
- Modify: `src/httpcommon.cpp` (define `save_api_token`; load `api_token` in `reload_user_creds`)
- Test: `tests/unit/test_httpcommon.cpp`

- [ ] **Step 1: Add the config field**

In `src/config.h`, inside `struct sunshine_t`, immediately after `std::string salt;` (line 271), add:

```cpp
    std::string api_token;  ///< Unsalted SHA-256 hash of the read-only API key; empty = disabled.
```

- [ ] **Step 2: Write the failing test**

Add to `tests/unit/test_httpcommon.cpp`:

```cpp
#include <src/config.h>
#include <filesystem>
#include <fstream>

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
```

- [ ] **Step 3: Run test to verify it fails**

Run: `ninja -C build`
Expected: compile failure — `http::save_api_token` not declared.

- [ ] **Step 4: Declare + implement persistence**

In `src/httpcommon.h`, inside `namespace http`, add:

```cpp
  /**
   * @brief Persist (or clear) the API token hash in the credentials file.
   * @param file The credentials file path.
   * @param token_hash The hash to store; pass "" to remove it.
   * @return 0 on success, -1 on error.
   */
  int save_api_token(const std::string &file, const std::string &token_hash);
```

In `src/httpcommon.cpp`, add after `save_user_creds`:

```cpp
  int save_api_token(const std::string &file, const std::string &token_hash) {
    nlohmann::json outputTree;
    if (fs::exists(file)) {
      try {
        std::ifstream in(file);
        in >> outputTree;
      } catch (std::exception &e) {
        BOOST_LOG(error) << "Couldn't read credentials file: "sv << e.what();
        return -1;
      }
    }

    if (token_hash.empty()) {
      outputTree.erase("api_token");
    } else {
      outputTree["api_token"] = token_hash;
    }

    try {
      std::ofstream out(file);
      out << outputTree.dump(4);
    } catch (std::exception &e) {
      BOOST_LOG(error) << "error writing to the credentials file: "sv << e.what();
      return -1;
    }
    return 0;
  }
```

In `src/httpcommon.cpp`, in `reload_user_creds`, after the line `config::sunshine.salt = inputTree.get<std::string>("salt");` add:

```cpp
      config::sunshine.api_token = inputTree.get<std::string>("api_token", "");
```

(`inputTree` here is a `boost::property_tree::ptree`; the two-arg `get` returns the default `""` when the key is absent — important for credentials files written before this feature existed.)

- [ ] **Step 5: Run test to verify it passes**

Run: `ninja -C build && ./build/tests/test_sunshine --gtest_filter='ApiTokenPersistence*'`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/config.h src/httpcommon.h src/httpcommon.cpp tests/unit/test_httpcommon.cpp
git commit -m "feat(api): persist read-only API token hash in credentials file"
```

---

## Task 3: Bearer auth path in `authenticate()`

**Files:**
- Modify: `src/confighttp.cpp` (add allowlist + Bearer block in `authenticate()`)

No unit test: `authenticate()` operates on live HTTPS request objects that aren't constructible in the unit harness. Verified manually in Task 6. The hashable/parsable logic it depends on is already unit-tested (Task 1).

- [ ] **Step 1: Add the path allowlist**

In `src/confighttp.cpp`, near the `sessionCookie` declaration (around line 64), add:

```cpp
  // Paths an API-key (read-only) request may reach. GET-only; deliberately excludes
  // /api/config and /api/logs (which can contain sensitive data).
  const std::set<std::string> TOKEN_ALLOWED_PATHS {
    "/api/clients/list",
  };
```

Ensure `#include <set>` is present at the top of the file (add it if not).

- [ ] **Step 2: Add the Bearer path to `authenticate()`**

In `src/confighttp.cpp`, in `authenticate()` (line 179), insert this block as the **very first thing** inside the function, before `if (!checkIPOrigin(response, request))`:

```cpp
    // Stateless API-key path (read-only). A Bearer token is the sole credential and
    // is honored only on GET requests to an allowlisted path. Origin gating is
    // intentionally bypassed: the token itself is the credential.
    auto authHeader = request->header.find("authorization");
    if (authHeader != request->header.end()) {
      auto token = http::extract_bearer_token(authHeader->second);
      if (!token.empty()) {
        if (!config::sunshine.api_token.empty() &&
            request->method == "GET" &&
            TOKEN_ALLOWED_PATHS.count(request->path) &&
            http::hash_api_token(token) == config::sunshine.api_token) {
          return true;
        }
        // Present but invalid, out-of-scope, or on a non-GET method: reject outright.
        send_unauthorized(response, request);
        return false;
      }
    }
```

(A browser never sends an `Authorization` header — it uses the `auth` cookie — so this block is inert for the web UI and falls through to the existing cookie logic.)

- [ ] **Step 3: Build**

Run: `ninja -C build`
Expected: builds cleanly.

- [ ] **Step 4: Commit**

```bash
git add src/confighttp.cpp
git commit -m "feat(api): accept read-only Bearer API key in authenticate()"
```

---

## Task 4: Token management endpoints (`/api/token`)

**Files:**
- Modify: `src/confighttp.cpp` (add `generateApiToken` + `revokeApiToken`; register routes)

- [ ] **Step 1: Add the handlers**

In `src/confighttp.cpp`, add near the other client handlers (e.g. after `getClients`, ~line 878):

```cpp
  /**
   * @brief Generate (or regenerate) the read-only API key.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * Cookie/admin auth only (POST is never honored by the Bearer path, so a key
   * cannot regenerate itself). Returns the plaintext key once; only its hash is stored.
   *
   * @api_examples{/api/token| POST| null}
   */
  void generateApiToken(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);

    std::string token = crypto::rand_alphabet(48);
    std::string token_hash = http::hash_api_token(token);
    if (http::save_api_token(config::sunshine.credentials_file, token_hash)) {
      bad_request(response, request, "Failed to save API token");
      return;
    }
    config::sunshine.api_token = token_hash;

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["token"] = token;  // shown once; not recoverable later
    send_response(response, output_tree);
  }

  /**
   * @brief Revoke the read-only API key.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/token| DELETE| null}
   */
  void revokeApiToken(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);

    if (http::save_api_token(config::sunshine.credentials_file, "")) {
      bad_request(response, request, "Failed to revoke API token");
      return;
    }
    config::sunshine.api_token = "";

    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }
```

- [ ] **Step 2: Register the routes**

In `src/confighttp.cpp`, in `start()`, alongside the other `server.resource[...]` registrations (after the `^/api/clients/...$` lines, ~line 1554), add:

```cpp
    server.resource["^/api/token$"]["POST"] = generateApiToken;
    server.resource["^/api/token$"]["DELETE"] = revokeApiToken;
```

- [ ] **Step 3: Build**

Run: `ninja -C build`
Expected: builds cleanly.

- [ ] **Step 4: Commit**

```bash
git add src/confighttp.cpp
git commit -m "feat(api): add POST/DELETE /api/token to manage the read-only key"
```

---

## Task 5: Web UI — "API Access" section

**Files:**
- Modify: `src_assets/common/assets/web/password.html`
- Modify: `src_assets/common/assets/web/public/assets/locale/en.json`

- [ ] **Step 1: Add i18n strings**

In `src_assets/common/assets/web/public/assets/locale/en.json`, add these keys **inside the existing `"password": { ... }` object** (line 494), matching the file's 4-space JSON style. Add a comma after the object's current last entry, then:

```json
    "api_access": "API Access",
    "api_access_desc": "Generate a read-only API key for headless integrations (e.g. Home Assistant). The key can read the client list (which device is connected) but cannot change anything.",
    "api_generate_key": "Generate API Key",
    "api_revoke_key": "Revoke",
    "api_key_once_warning": "Copy this key now — it will not be shown again.",
    "api_key_copy": "Copy"
```

(They live under `password` because the hosting page resolves keys as `$t('password.*')`.)

- [ ] **Step 2: Add the UI card**

In `src_assets/common/assets/web/password.html`, add this block right after the closing `</div>` of the existing credentials card (after line ~64, before the `</div>` that closes the page container):

```html
      <div class="card d-flex p-4 flex-column mt-4">
        <h2 class="mb-2">{{ $t('password.api_access') }}</h2>
        <p class="text-muted">{{ $t('password.api_access_desc') }}</p>
        <div v-if="apiKey" class="alert alert-warning d-flex flex-column">
          <span class="mb-2">{{ $t('password.api_key_once_warning') }}</span>
          <div class="d-flex flex-row">
            <input class="form-control me-2" :value="apiKey" readonly />
            <button class="btn btn-secondary" @click="copyApiKey">{{ $t('password.api_key_copy') }}</button>
          </div>
        </div>
        <div class="d-flex flex-row">
          <button class="btn btn-primary me-2" @click="generateApiKey">{{ $t('password.api_generate_key') }}</button>
          <button class="btn btn-danger" @click="revokeApiKey">{{ $t('password.api_revoke_key') }}</button>
        </div>
        <div v-if="apiError" class="alert alert-danger mt-2">{{ apiError }}</div>
      </div>
```

- [ ] **Step 3: Add Vue data + methods**

In the same file's `<script>` block: add to the `data()` return object (after `passwordData: {...},`):

```js
        apiKey: null,
        apiError: null,
```

And add these methods to the `methods: {` object (after the `save()` method):

```js
      generateApiKey() {
        this.apiError = null;
        fetch("./api/token", { credentials: 'include', method: 'POST' })
          .then((r) => r.json())
          .then((rj) => {
            if (rj.status === true) {
              this.apiKey = rj.token;
            } else {
              this.apiError = rj.error || "Failed to generate key";
            }
          })
          .catch(() => { this.apiError = "Internal Server Error"; });
      },
      revokeApiKey() {
        this.apiError = null;
        this.apiKey = null;
        fetch("./api/token", { credentials: 'include', method: 'DELETE' })
          .then((r) => r.json())
          .then((rj) => { if (rj.status !== true) this.apiError = rj.error || "Failed to revoke key"; })
          .catch(() => { this.apiError = "Internal Server Error"; });
      },
      copyApiKey() {
        if (this.apiKey) navigator.clipboard.writeText(this.apiKey);
      },
```

- [ ] **Step 4: Build the web UI + full app**

Run: `npm install && ninja -C build`
Expected: web UI builds (the `web-ui` CMake target runs the Vite build); no errors.

- [ ] **Step 5: Commit**

```bash
git add src_assets/common/assets/web/password.html src_assets/common/assets/web/public/assets/locale/en.json
git commit -m "feat(web): add API Access key management to the password page"
```

---

## Task 6: Manual end-to-end verification

**Files:** none (verification only). Per `.github/CONTRIBUTING.md`, manually verify behavior.

- [ ] **Step 1: Run Apollo and set credentials**

Start the built binary, open the web UI, set an admin username/password if not already set.

- [ ] **Step 2: Generate a key**

On the password page, click **Generate API Key**; copy the shown key into `$KEY`. Set `$HOST` to the config server (default `https://localhost:47990`).

- [ ] **Step 3: Verify the happy path (allowlisted GET)**

Run:
```bash
curl -k -H "Authorization: Bearer $KEY" "$HOST/api/clients/list"
```
Expected: `200` with `{"named_certs":[...{"name":...,"connected":true|false}...],"status":true}`.

- [ ] **Step 4: Verify read-only + scope enforcement**

```bash
curl -k -s -o /dev/null -w "%{http_code}\n" -H "Authorization: Bearer $KEY" "$HOST/api/config"      # expect 401 (not allowlisted)
curl -k -s -o /dev/null -w "%{http_code}\n" -H "Authorization: Bearer $KEY" -X POST "$HOST/api/restart"  # expect 401 (non-GET)
curl -k -s -o /dev/null -w "%{http_code}\n" -H "Authorization: Bearer wrong" "$HOST/api/clients/list"    # expect 401 (bad key)
```
Expected: `401` for all three.

- [ ] **Step 5: Verify origin bypass**

From a different LAN host (the Home Assistant box, or any other machine on the LAN), with `origin_web_ui_allowed` at its default:
```bash
curl -k -H "Authorization: Bearer $KEY" "https://<apollo-ip>:47990/api/clients/list"
```
Expected: `200` (no 403 — the key bypasses origin), whereas the same URL without the header returns `403`.

- [ ] **Step 6: Verify revoke + browser coexistence**

- Click **Revoke**, then re-run Step 3 → expect `401`.
- Generate a new key, keep a browser tab logged in, and poll `/api/clients/list` with the key in a loop → the browser session stays logged in (no eviction).

- [ ] **Step 7: Commit any doc/notes if needed** (otherwise nothing to commit).

---

## Task 7: Documentation

**Files:**
- Modify: `docs/api.md` (add `/api/token` entries + a Home Assistant usage note)

- [ ] **Step 1: Document the endpoint and usage**

Add to `docs/api.md`, near the other `/api/...` entries:

```markdown
## POST /api/token
@copydoc confighttp::generateApiToken()

## DELETE /api/token
@copydoc confighttp::revokeApiToken()

## Read-only API key (headless integrations)

Generate a key on the web UI's password page, then send it as a Bearer token:

    curl -H "Authorization: Bearer <key>" https://<host>:47990/api/clients/list

The key is read-only: it is accepted only on GET requests to allowlisted paths
(currently `/api/clients/list`) and bypasses the Web UI origin restriction. Use the
`named_certs[].connected` flags to drive automations (e.g. in Home Assistant).
```

- [ ] **Step 2: Commit**

```bash
git add docs/api.md
git commit -m "docs(api): document read-only API key + /api/token endpoints"
```

---

## Self-Review

**Spec coverage:**
- Credential (generated, hashed unsalted, shown once, stored in creds file) → Tasks 2, 4 ✓
- Bearer path in existing `authenticate()`; read-only via GET+allowlist; origin bypass → Task 3 ✓
- Allowlist excludes `/api/config`, `/api/logs` → Task 3 (`TOKEN_ALLOWED_PATHS`) ✓
- Management routes `POST`/`DELETE /api/token`, cookie-only → Task 4 ✓
- Web UI generate/revoke/show-once → Task 5 ✓
- HA usage docs → Task 7 ✓
- Error-handling matrix (401 cases, disabled-when-empty) → Task 3 + verified Task 6 ✓
- "Future tightening" (field redaction) intentionally **not** implemented (spec marks it out of scope) ✓

**Type/name consistency:** `http::hash_api_token`, `http::extract_bearer_token`, `http::save_api_token`, `config::sunshine.api_token`, `TOKEN_ALLOWED_PATHS`, `generateApiToken`, `revokeApiToken`, `apiKey`/`apiError` (web) — used consistently across tasks.

**Placeholder scan:** none — every code/test step contains complete content.

**i18n note:** only `en.json` (the source locale) is updated; other locales flow through Crowdin (`crowdin.yml`) and will fall back to English until translated.
```
