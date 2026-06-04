# API

Sunshine has a RESTful API which can be used to interact with the service.

Unless otherwise specified, authentication is required for all API calls. Two mechanisms are
supported:

- **Session cookie** — authenticate the admin username and password via [POST /api/login](#post-apilogin).
  A successful login returns a session cookie that the browser (or client) sends on subsequent
  requests. This is the mechanism used by the Web UI.
- **Read-only API key** — send a Bearer token for headless integrations. See
  [Read-only API key (headless integrations)](#read-only-api-key-headless-integrations) below.

@htmlonly
<script src="api.js"></script>
@endhtmlonly

## GET /api/apps
@copydoc confighttp::getApps()

## POST /api/apps
@copydoc confighttp::saveApp()

## POST /api/apps/close
@copydoc confighttp::closeApp()

## DELETE /api/apps/{index}
@copydoc confighttp::deleteApp()

## GET /api/clients/list
@copydoc confighttp::getClients()

## POST /api/clients/unpair
@copydoc confighttp::unpair()

## POST /api/clients/unpair-all
@copydoc confighttp::unpairAll()

## GET /api/config
@copydoc confighttp::getConfig()

## GET /api/configLocale
@copydoc confighttp::getLocale()

## POST /api/config
@copydoc confighttp::saveConfig()

## POST /api/covers/upload
@copydoc confighttp::uploadCover()

## GET /api/logs
@copydoc confighttp::getLogs()

## POST /api/login
@copydoc confighttp::login()

Send the admin credentials as a JSON body with `Content-Type: application/json`:

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"username": "<username>", "password": "<password>"}' \
  https://<host>:47990/api/login
```

On success the response sets a session cookie (`auth`) that is valid for the configured
session lifetime; send it on subsequent API requests to authenticate as the admin user.
Invalid credentials return `401 Unauthorized`.

## POST /api/password
@copydoc confighttp::savePassword()

## POST /api/pin
@copydoc confighttp::savePin()

## POST /api/reset-display-device-persistence
@copydoc confighttp::resetDisplayDevicePersistence()

## POST /api/restart
@copydoc confighttp::restart()

## POST /api/token
@copydoc confighttp::generateApiToken()

## DELETE /api/token
@copydoc confighttp::revokeApiToken()

## Read-only API key (headless integrations)

For headless clients such as Home Assistant, generate an API key on the Web UI's
password page, then send it as a Bearer token:

```bash
curl -H "Authorization: Bearer <key>" https://<host>:47990/api/clients/list
```

The key is read-only: it is accepted only on `GET` requests to allowlisted paths
(currently `/api/clients/list`) and bypasses the Web UI origin restriction, so it works
from another LAN host without exposing the rest of the Web UI. Use the
`named_certs[].connected` flags in the response to drive automations.

<div class="section_buttons">

| Previous                                    |                                  Next |
|:--------------------------------------------|--------------------------------------:|
| [Performance Tuning](performance_tuning.md) | [Troubleshooting](troubleshooting.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
