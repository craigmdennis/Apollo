# Read-only API key for headless integrations (Home Assistant)

**Date:** 2026-06-04
**Status:** Approved (design)
**Author:** Craig Dennis (with Claude Code)

## Problem

Home Assistant needs to query Apollo for **which paired device is currently
connected/streaming**, in order to drive automations. Today this is effectively
impossible:

1. **403 Forbidden.** The config server (`confighttp`) gates every request through
   `checkIPOrigin()` against `origin_web_ui_allowed`. Its effective default is `PC`
   (localhost only — `net::from_enum_string("")` returns `PC`), so a Home Assistant
   box on the LAN is rejected.
2. **Stateful single-session auth.** `authenticate()` (`src/confighttp.cpp:179`)
   accepts **only** a session cookie set by `POST /api/login`. There is exactly one
   global `sessionCookie` (`src/confighttp.cpp:64`), so a headless client logging in
   evicts the admin's browser session and vice-versa. Any cookie-replay workaround
   "fights" the browser.

### Root-cause context (verified in git history)

Upstream **Sunshine authenticated this same `/api/*` surface with stateless HTTP
Basic Auth** (`Authorization: Basic ...`, realm `"Sunshine Gamestream Host"`).
Apollo commit **`652661ea` "Change login from http basic auth to cookies"**
(2024-08-30) replaced Basic Auth with the cookie/login model and **never restored a
stateless path for non-browser clients**. The file-level `@todo Authentication,
better handling of routes common to nvhttp, cleanup` (`src/confighttp.cpp:5`) is the
residue of that half-finished migration.

**The data already exists.** `nvhttp::get_all_clients()` (`src/nvhttp.cpp:1015`)
already returns one node per paired device with `name`, `uuid`, and a live
`connected` boolean (derived from `rtsp_stream::get_all_session_uuids()`), and it is
already exposed at `GET /api/clients/list`. The web UI consumes it in `pin.html`.

So the only genuinely missing piece is a **stateless authentication primitive** —
not a new data endpoint.

## Goals

- A headless integration can authenticate **without** a browser session and without
  fighting it.
- Re-use the **existing** `/api/clients/list` data — no new data/status endpoint.
- The credential is **read-only** and least-privilege.
- Mirror existing secret-handling conventions (hashed at rest, shown once).

## Non-goals

- Write/control access from the key (launch, close, config changes, unpair). Out of
  scope; future "write-scoped tokens" if ever needed.
- Multiple named keys / per-key permissions. Single key for now (YAGNI).
- Exposing current-app / server BUSY-FREE state to the key. The stated requirement is
  *which device is connected*, which `/api/clients/list` answers. Adding app/state to
  that payload later is a small, separate change.
- Restoring Basic Auth for upstream-Sunshine compatibility (considered, rejected in
  favor of a single modern Bearer scheme).

## Design

### 1. The credential — a read-only API key

- High-entropy key generated with `crypto::rand_alphabet(48)` (same generator already
  used for session cookies).
- **Stored hashed**, never in plaintext, in the existing credentials file
  (`config::sunshine.credentials_file`) as a new field `api_token`, computed as
  `util::hex(crypto::hash(token))` — an **unsalted** SHA-256. The admin password is
  salted (`src/httpcommon.cpp:87`) because it is low-entropy; an API key is 48 random
  characters, so a salt adds nothing and would introduce a bug: `save_user_creds`
  regenerates the shared `salt` on every password change (`src/httpcommon.cpp:84`),
  which would silently invalidate a salt-coupled key. Unsalted hashing decouples the
  key's lifetime from the password.
- Loaded into a new `config::sunshine.api_token` (the hash; empty string = feature
  disabled) by `reload_user_creds()` (`src/httpcommon.cpp:118`).
- The plaintext key is shown **once**, at generation time, in the web UI. A leaked
  credentials file cannot reveal it.

### 2. Authentication — extend the existing gate

`authenticate()` gains a stateless Bearer path, checked **before** the cookie/origin
logic:

```
authenticate(response, request, needsRedirect=false):
    # --- stateless API-key path (read-only) ---
    if request.method == "GET"
       and request.path is in TOKEN_ALLOWED_PATHS
       and an "Authorization: Bearer <key>" header is present:
        if api_token configured
           and hex(hash(key)) == config.api_token:    # unsalted; matches cookie-path compare style
            return true            # authenticated; origin check intentionally bypassed
        else:
            send_unauthorized(); return false   # present-but-invalid key => 401, no fall-through

    # --- existing cookie path (unchanged) ---
    if not checkIPOrigin(...): return false
    ... existing session-cookie validation ...
```

Key properties:

- **Read-only by construction.** The Bearer key is honored **only on `GET`** and
  **only for an allowlist of safe paths** (`TOKEN_ALLOWED_PATHS`, initially
  `{"/api/clients/list"}`). It is never honored on `POST`/`DELETE`/etc., so it cannot
  change configuration, launch/close apps, unpair, or generate/revoke keys. No
  separate write-guard logic is needed — the restriction is structural.
- **Least-privilege read scope.** The allowlist deliberately **excludes** `/api/config`
  and `/api/logs`, which can contain sensitive data. It is a small, extensible set.
- **Origin bypass is intentional.** A valid key is a strong secret and is sufficient
  authentication, so the key path does not call `checkIPOrigin()`. This is what fixes
  the LAN 403 without forcing the user to open the whole admin UI to the LAN. The
  browser/cookie path keeps its origin gating unchanged.
- The compared value is a SHA-256 hash (not the raw key), matching the existing
  cookie-path comparison style (`src/confighttp.cpp:209`); a timing side-channel on a
  hash comparison is not practically exploitable (would require a SHA-256 preimage).

### 3. Key management — one admin-only resource

A single new route on `confighttp`, cookie-authenticated (admin, in browser only —
the key cannot manage itself because management is non-GET):

- `POST /api/token` → generate (or regenerate) the key. Persists the new hash via a
  `save_api_token()` helper (mirrors `save_user_creds`) and returns
  `{ "status": true, "token": "<plaintext, shown once>" }`.
- `DELETE /api/token` → revoke. Clears the stored hash; responds `{ "status": true }`.

### 4. Web UI

A small "API Access" section (in the existing config/account area):

- A **Generate API Key** button (`POST /api/token`) that reveals the key once, with a
  copy button and a "store it now, it won't be shown again" warning.
- A **Revoke** button (`DELETE /api/token`), shown when a key exists.
- Inline usage hint, e.g.:
  `curl -H "Authorization: Bearer <key>" https://<host>:47990/api/clients/list`

### 5. Home Assistant usage (documentation only)

A REST sensor polls `GET /api/clients/list` with the Bearer header; the
`named_certs[].connected` booleans (keyed by `name`/`uuid`) drive automations. No
credentials stored in HA — just the key.

## Data flow

```
Home Assistant ──GET /api/clients/list──────────────────────────┐
   Authorization: Bearer <key>                                   │
                                                                 ▼
                                        confighttp::authenticate()
                                          ├ method==GET & path allowlisted & Bearer present?
                                          │     └ hash(key+salt) == config.api_token ? ──► authorized
                                          └ else ► existing cookie+origin path
                                                                 │
                                                                 ▼
                              nvhttp::get_all_clients() ── named_certs[] {name,uuid,connected,...}
```

## Error handling

| Condition | Response |
|---|---|
| Valid key, GET, allowlisted path | `200` + existing JSON |
| Valid key, GET, **non-allowlisted** path | `401` (key not honored there; no cookie → unauthorized) |
| Valid key on non-GET (e.g. tries `POST /api/config`) | `401` (key never honored on writes) |
| Present but invalid Bearer key | `401`, no fall-through to cookie |
| No key configured (`api_token` empty) | Bearer path disabled; behaves as today |
| Browser request (cookie) | Unchanged, including origin gating |

## Security considerations / tradeoffs

- **Origin bypass for the key is deliberate** and documented; the key is the
  credential. The admin UI's origin gating is untouched.
- **`/api/clients/list` still includes each client's `do`/`undo` shell commands and
  permission bits.** A read-only key can therefore *read* (not execute) the admin's
  own configured commands. Accepted: the key is admin-generated for the admin's own
  automation, read-only, and over HTTPS. **Documented future tightening:** redact
  `do`/`undo`/`perm` from the `/api/clients/list` response when the request was
  authenticated via API key.
- Key stored hashed (unsalted SHA-256; the key is high-entropy), shown once.
  `confighttp` is HTTPS-only, so the Bearer header is never sent in clear.

## Testing

GoogleTest, building on `tests/unit/test_httpcommon.cpp` / pairing-test patterns
(`-DBUILD_TESTS=ON`, run `./build/tests/test_sunshine --gtest_filter=...`):

1. Valid key + `GET` allowlisted path → authenticated.
2. Invalid key → `401`, does not fall through to cookie.
3. Valid key on a non-allowlisted GET (e.g. `/api/config`) → not authenticated.
4. Valid key on a non-GET → not authenticated (read-only enforced).
5. No key configured → Bearer path inert; cookie behavior unchanged.
6. Key path is not subject to `checkIPOrigin` (LAN-origin request authenticates).
7. `save_api_token` / `reload_user_creds` round-trip stores and loads the hash;
   `DELETE` clears it.

Per `.github/CONTRIBUTING.md` ("Don't trust AI generated tests"), these tests will be
manually verified.

## Files touched (anticipated)

- `src/config.h` / `src/config.cpp` — add `api_token` to the `sunshine_t` config.
- `src/httpcommon.cpp` / `.h` — `save_api_token()`, load `api_token` in
  `reload_user_creds()`.
- `src/confighttp.cpp` — Bearer path in `authenticate()`; `TOKEN_ALLOWED_PATHS`;
  `generateApiToken` / `revokeApiToken` handlers; route registration for `/api/token`.
- `src_assets/common/assets/web/` — API Access UI section + i18n strings.
- `docs/` — document the endpoint/usage; `tests/unit/` — tests above.

## Future work (out of scope now)

- Redact sensitive fields from `/api/clients/list` for key-authenticated requests.
- Add current-app / server state to the read-only payload if automations need it.
- Multiple named keys and/or write-scoped keys.
