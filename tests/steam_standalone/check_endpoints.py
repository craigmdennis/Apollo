"""Pair with Apollo as a Moonlight client and check the Steam activity endpoints.

Usage:
  python3 check_endpoints.py pair [--host H] [--name NAME]
  python3 check_endpoints.py check [--host H] [--expect shared|withheld|signed-out]
  python3 check_endpoints.py serverinfo [--host H]
  python3 check_endpoints.py watch [--host H] [--every SECONDS]
Requires: pip install cryptography requests

pair prints a PIN. Enter it on the Apollo PIN page, then turn on the share_activity
permission for the new client. The client key, certificate and pinned host certificate
are saved beside this script and reused by the other commands.

check compares Apollo's answers with what this script reads from the Steam files and
the registry itself, prints PASS or FAIL per check, and exits 1 on any failure. Run it
on the host, as the signed-in user, so the expected values come from the same data.
"""
import argparse
import datetime
import glob
import hashlib
import json
import os
import re
import secrets
import ssl
import sys
import time
import uuid
import xml.etree.ElementTree as ET

import requests
import urllib3

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.x509.oid import NameOID
from requests.adapters import HTTPAdapter

here = os.path.dirname(os.path.abspath(__file__))
key_file = os.path.join(here, ".check_endpoints.key")
cert_file = os.path.join(here, ".check_endpoints.crt")
state_file = os.path.join(here, ".check_endpoints.json")

STEAM_ID_BASE = 76561197960265728
MAX_ART_BYTES = 8 * 1024 * 1024
MAX_WALLPAPER_BYTES = 20 * 1024 * 1024
ART_NAMES = {
    "hero": ["library_hero.jpg"],
    "logo": ["logo.png"],
    "capsule": ["library_600x900.jpg", "library_capsule.jpg", "header.jpg", "library_header.jpg"],
}

failures = []


def report(ok, label, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}  {label}{'  -- ' + detail if detail else ''}")
    if not ok:
        failures.append(label)


# --- Client identity and HTTPS -------------------------------------------------------


def load_or_create_identity():
    if os.path.exists(key_file) and os.path.exists(cert_file):
        with open(key_file, "rb") as handle:
            key = serialization.load_pem_private_key(handle.read(), None)
        with open(cert_file, "rb") as handle:
            return key, x509.load_pem_x509_certificate(handle.read())
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "NVIDIA GameStream Client")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
            .public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=3650))
            .sign(key, hashes.SHA256()))
    with open(key_file, "wb") as handle:
        handle.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL,
                                       serialization.NoEncryption()))
    with open(cert_file, "wb") as handle:
        handle.write(cert.public_bytes(serialization.Encoding.PEM))
    return key, cert


class PinnedAdapter(HTTPAdapter):
    """Trust only the host certificate received while pairing. The certificate is
    self-signed and carries no host name, so the fingerprint replaces both checks."""

    def __init__(self, fingerprint):
        self.fingerprint = fingerprint
        super().__init__()

    def init_poolmanager(self, *args, **kwargs):
        kwargs["assert_fingerprint"] = self.fingerprint
        kwargs["cert_reqs"] = "CERT_NONE"
        return super().init_poolmanager(*args, **kwargs)


def load_state():
    if not os.path.exists(state_file):
        sys.exit("Not paired. Run: python3 check_endpoints.py pair")
    with open(state_file) as handle:
        return json.load(handle)


def https_session(state):
    http = requests.Session()
    http.mount("https://", PinnedAdapter(state["server_fingerprint"]))
    http.verify = False  # The adapter's fingerprint pin does the verification.
    http.cert = (cert_file, key_file)
    return http


def https_base(args):
    return f"https://{args.host}:{args.port - 5}"


def xml_root(text):
    root = ET.fromstring(text)
    status = root.get("status_code")
    if status != "200":
        sys.exit(f"Host answered status {status}: {root.get('status_message')}")
    return root


# --- Pairing -------------------------------------------------------------------------


def aes_ecb(key, data, encrypt):
    cipher = Cipher(algorithms.AES(key), modes.ECB())
    worker = cipher.encryptor() if encrypt else cipher.decryptor()
    return worker.update(data) + worker.finalize()


def pair(args):
    key, cert = load_or_create_identity()
    cert_pem = cert.public_bytes(serialization.Encoding.PEM)
    unique_id = secrets.token_hex(8)
    pin = f"{secrets.randbelow(10000):04d}"
    salt = secrets.token_bytes(16)
    base = f"http://{args.host}:{args.port}/pair"
    common = {"uniqueid": unique_id, "uuid": uuid.uuid4().hex, "devicename": args.name, "updateState": 1}

    print(f"PIN: {pin}")
    print(f"Enter it on the Apollo PIN page (https://{args.host}:{args.port + 1}/pin) within 5 minutes.")
    reply = requests.get(base, params={**common, "phrase": "getservercert", "salt": salt.hex(),
                                       "clientcert": cert_pem.hex()}, timeout=300)
    root = xml_root(reply.text)
    if root.findtext("paired") != "1":
        sys.exit("Host refused the pairing request")
    server_cert = x509.load_pem_x509_certificate(bytes.fromhex(root.findtext("plaincert")))
    aes_key = hashlib.sha256(salt + pin.encode()).digest()[:16]

    client_challenge = secrets.token_bytes(16)
    root = xml_root(requests.get(base, params={**common, "clientchallenge": aes_ecb(aes_key, client_challenge, True).hex()},
                                 timeout=30).text)
    decrypted = aes_ecb(aes_key, bytes.fromhex(root.findtext("challengeresponse")), False)
    server_hash, server_challenge = decrypted[:32], decrypted[32:48]

    client_secret = secrets.token_bytes(16)
    client_hash = hashlib.sha256(server_challenge + cert.signature + client_secret).digest()
    root = xml_root(requests.get(base, params={**common, "serverchallengeresp": aes_ecb(aes_key, client_hash, True).hex()},
                                 timeout=30).text)
    pairing_secret = bytes.fromhex(root.findtext("pairingsecret"))
    server_secret, server_signature = pairing_secret[:16], pairing_secret[16:]

    # A wrong PIN shows up here: the server's hash will not match.
    if hashlib.sha256(client_challenge + server_cert.signature + server_secret).digest() != server_hash:
        sys.exit("The host's challenge hash does not match. The PIN was probably entered wrong.")
    server_cert.public_key().verify(server_signature, server_secret, padding.PKCS1v15(), hashes.SHA256())

    signature = key.sign(client_secret, padding.PKCS1v15(), hashes.SHA256())
    root = xml_root(requests.get(base, params={**common, "clientpairingsecret": (client_secret + signature).hex()},
                                 timeout=30).text)
    if root.findtext("paired") != "1":
        sys.exit("The host rejected the pairing secret")

    fingerprint = hashlib.sha256(server_cert.public_bytes(serialization.Encoding.DER)).hexdigest()
    state = {"unique_id": unique_id, "name": args.name, "server_fingerprint": fingerprint}
    with open(state_file, "w") as handle:
        json.dump(state, handle, indent=2)
    root = xml_root(https_session(state).get(f"{https_base(args)}/pair",
                                             params={**common, "phrase": "pairchallenge"}, timeout=30).text)
    if root.findtext("paired") != "1":
        sys.exit("Paired, but the HTTPS pair challenge failed")
    print(f"Paired as '{args.name}'. Now turn on share_activity for it on the Apollo PIN page.")


# --- Expected values, read independently of Apollo ------------------------------------


def parse_vdf(text):
    """Minimal KeyValues reader: quoted strings, braces, // comments, backslash escapes."""
    stack = [{}]
    pending = None
    for token in re.finditer(r'//[^\n]*|"(?P<quoted>(?:\\.|[^"\\])*)"|(?P<brace>[{}])', text):
        quoted, brace = token.group("quoted"), token.group("brace")
        if brace == "{":
            child = {}
            stack[-1][(pending or "").lower()] = child
            stack.append(child)
            pending = None
        elif brace == "}":
            if len(stack) > 1:
                stack.pop()
        elif quoted is not None:
            value = re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)), quoted)
            if pending is None:
                pending = value
            else:
                stack[-1][pending.lower()] = value
                pending = None
    return stack[0]


def read_vdf(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return parse_vdf(handle.read())


def registry(hive, path, name):
    import winreg
    try:
        with winreg.OpenKey(hive, path, 0, winreg.KEY_READ | getattr(winreg, "KEY_WOW64_32KEY", 0)) as key:
            return winreg.QueryValueEx(key, name)[0]
    except OSError:
        return None


def expected_activity():
    import winreg
    root = registry(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam", "InstallPath")
    if not root or not os.path.isdir(root):
        return None
    account = registry(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam\ActiveProcess", "ActiveUser") or 0
    if not account:
        users = read_vdf(os.path.join(root, "config", "loginusers.vdf")).get("users", {})
        ranked = sorted(users.items(), key=lambda kv: (kv[1].get("mostrecent") == "1", kv[1].get("autologin") == "1",
                                                       int(kv[1].get("timestamp", "0") or 0)))
        account = int(ranked[-1][0]) - STEAM_ID_BASE if ranked else 0

    libraries = [root]
    try:
        for entry in read_vdf(os.path.join(root, "steamapps", "libraryfolders.vdf")).get("libraryfolders", {}).values():
            if isinstance(entry, dict) and entry.get("path"):
                libraries.append(entry["path"])
    except OSError:
        pass

    def name_of(app_id):
        for library in libraries:
            manifest = os.path.join(library, "steamapps", f"appmanifest_{app_id}.acf")
            if os.path.exists(manifest):
                return read_vdf(manifest).get("appstate", {}).get("name")
        return None

    games = []
    if account:
        config = os.path.join(root, "userdata", str(account), "config", "localconfig.vdf")
        apps = read_vdf(config).get("userlocalconfigstore", {}).get("software", {}).get("valve", {}) \
            .get("steam", {}).get("apps", {}) if os.path.exists(config) else {}
        for app_id, values in apps.items():
            at = int(values.get("lastplayed", "0") or 0) if isinstance(values, dict) else 0
            name = name_of(app_id) if at > 0 else None
            if name:
                games.append({"appId": int(app_id), "name": name, "at": at})
    games.sort(key=lambda g: g["at"], reverse=True)

    running = registry(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "RunningAppID") or 0
    return {"root": root, "account": str(account + STEAM_ID_BASE) if account else None, "recent": games[:5],
            "runningAppId": running or None, "runningName": name_of(running) if running else None}


def art_on_disk(root, app_id, kind):
    cache = os.path.join(root, "appcache", "librarycache")
    for name in ART_NAMES[kind]:
        hits = [os.path.join(cache, f"{app_id}_{name}")] + glob.glob(os.path.join(cache, str(app_id), name)) \
            + glob.glob(os.path.join(cache, str(app_id), "*", name))
        for hit in hits:
            if os.path.isfile(hit) and os.path.getsize(hit) <= MAX_ART_BYTES:
                return hit
    return None


# --- Image checks --------------------------------------------------------------------


def sniff(data):
    if data[:2] == b"\xff\xd8":
        return "image/jpeg"
    if data[:4] == b"\x89PNG":
        return "image/png"
    if data[:2] == b"BM":
        return "image/bmp"
    if data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return "image/webp"
    return None


def dimensions(data):
    if data[:4] == b"\x89PNG":
        return int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")
    if data[:2] == b"BM":
        return int.from_bytes(data[18:22], "little"), abs(int.from_bytes(data[22:26], "little", signed=True))
    if data[:2] == b"\xff\xd8":
        i = 2
        while i + 9 < len(data):
            if data[i] != 0xFF:
                i += 1
                continue
            marker, length = data[i + 1], int.from_bytes(data[i + 2:i + 4], "big")
            if marker in (0xC0, 0xC1, 0xC2):
                return int.from_bytes(data[i + 7:i + 9], "big"), int.from_bytes(data[i + 5:i + 7], "big")
            i += 2 + length
    return None


def check_image(reply, label, limit):
    data = reply.content
    kind = sniff(data)
    size = dimensions(data)
    ok = (reply.status_code == 200 and kind is not None and reply.headers.get("Content-Type") == kind
          and len(data) <= limit)
    report(ok, label, f"{reply.status_code} {reply.headers.get('Content-Type')} {len(data)} bytes"
                      f"{' %dx%d' % size if size else ''}")


# --- Commands ------------------------------------------------------------------------


def get(http, url):
    return http.get(url, timeout=30)


def raw_status(http, base, path):
    """Send the path byte for byte. requests resolves ".." while preparing a URL, so
    the prepared URL is replaced after preparation."""
    prepared = http.prepare_request(requests.Request("GET", base + "/"))
    prepared.url = base + path
    return http.send(prepared, timeout=30).status_code


def serverinfo_fields(http, base):
    root = ET.fromstring(get(http, f"{base}/serverinfo").text)
    return {name: root.findtext(name) for name in ("hostname", "state", "currentgame", "activeclient")}


def check(args):
    state = load_state()
    http = https_session(state)
    base = https_base(args)
    reply = get(http, f"{base}/moonlight/steam")
    report(reply.status_code == 200 and reply.headers.get("Content-Type") == "application/json",
           "activity answers 200 application/json", f"{reply.status_code} {reply.headers.get('Content-Type')}")
    body = reply.json()
    print(json.dumps(body, indent=2))

    if args.expect == "withheld":
        report(body == {"version": 1, "sharing": False}, "activity is withheld", json.dumps(body))
        for kind in ("hero", "logo", "capsule"):
            r = get(http, f"{base}/moonlight/steam/art/440/{kind}")
            report(r.status_code == 404, f"art/{kind} is 404 without permission", str(r.status_code))
        r = get(http, f"{base}/moonlight/wallpaper")
        report(r.status_code == 404, "wallpaper is 404 without permission", str(r.status_code))
        info = serverinfo_fields(http, base)
        report(not info["activeclient"], "serverinfo hides activeclient", repr(info["activeclient"]))
        return

    expected = expected_activity()
    if expected is None:
        report(body == {"version": 1, "steamInstalled": False}, "activity says Steam is not installed", json.dumps(body))
        return

    keys = {"version", "steamInstalled", "account", "runningAppId", "runningName", "lastPlayed", "recent", "updatedAt"}
    report(set(body) == keys and body["version"] == 1 and body["steamInstalled"] is True,
           "activity has the full contract shape", ", ".join(sorted(set(body) ^ keys)))
    report(body.get("account") == expected["account"], "account matches the Steam files",
           f"got {body.get('account')}, expected {expected['account']}")
    report(body.get("recent") == expected["recent"], "recent matches localconfig and manifests",
           f"expected {[g['appId'] for g in expected['recent']]}")
    report(body.get("lastPlayed") == (expected["recent"][0] if expected["recent"] else None),
           "lastPlayed is the newest installed game")
    report((body.get("runningAppId") is None) == (body.get("runningName") is None),
           "runningName is null exactly when runningAppId is null")
    if args.expect == "signed-out":
        report(body.get("runningAppId") is None, "runningAppId is null while signed out")
    else:
        report(body.get("runningAppId") == expected["runningAppId"], "runningAppId matches the registry",
               f"got {body.get('runningAppId')}, expected {expected['runningAppId']} (cached up to 30 s)")
    report(abs(time.time() - body.get("updatedAt", 0)) < 120, "updatedAt is recent")

    for game in body.get("recent", []):
        for kind in ("hero", "logo", "capsule"):
            label = f"art {game['appId']}/{kind} ({game['name']})"
            r = get(http, f"{base}/moonlight/steam/art/{game['appId']}/{kind}")
            on_disk = art_on_disk(expected["root"], game["appId"], kind)
            if args.expect == "signed-out":
                report(r.status_code == 404, label + " is 404 while signed out", str(r.status_code))
            elif on_disk:
                check_image(r, label, MAX_ART_BYTES)
                report(r.headers.get("Cache-Control") == "max-age=3600", label + " Cache-Control",
                       r.headers.get("Cache-Control", ""))
            else:
                report(r.status_code == 404, label + " is 404 (no file on disk)", str(r.status_code))

    for path in ("art/abc/hero", "art/440/banner", "art/12345678901/hero", "art/0/hero", "art/440/hero/../logo",
                 "art/440/../../steam", "art/440%2F..%2F441/hero", "art/440/hero/", "art/440/HERO"):
        status = raw_status(http, base, f"/moonlight/steam/{path}")
        report(status == 404, f"{path} is 404", str(status))

    r = get(http, f"{base}/moonlight/wallpaper")
    if args.expect == "signed-out":
        report(r.status_code == 404, "wallpaper is 404 while signed out", str(r.status_code))
    else:
        check_image(r, "wallpaper", MAX_WALLPAPER_BYTES)
        report(r.headers.get("Cache-Control") == "no-cache", "wallpaper Cache-Control", r.headers.get("Cache-Control", ""))


def serverinfo(args):
    state = load_state()
    print(json.dumps(serverinfo_fields(https_session(state), https_base(args)), indent=2))


def watch(args):
    state = load_state()
    http = https_session(state)
    base = https_base(args)
    last = None
    print(f"Polling every {args.every} s. Press Ctrl+C to stop. Activity is cached for up to 30 s on the host.")
    while True:
        try:
            body = get(http, f"{base}/moonlight/steam").json()
            info = serverinfo_fields(http, base)
            now = {"runningAppId": body.get("runningAppId"), "runningName": body.get("runningName"),
                   "lastPlayed": (body.get("lastPlayed") or {}).get("name"), "state": info["state"],
                   "currentgame": info["currentgame"], "activeclient": info["activeclient"]}
        except requests.RequestException as error:
            now = {"error": type(error).__name__}
        if now != last:
            print(time.strftime("%H:%M:%S"), json.dumps(now))
            last = now
        time.sleep(args.every)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["pair", "check", "serverinfo", "watch"])
    # Not "localhost": Windows tries IPv6 first, and Apollo listens on IPv4 only, which
    # adds about 2 s to every connection and sometimes stalls the handshake.
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=47989, help="Apollo's HTTP port; HTTPS is this minus 5")
    parser.add_argument("--name", default="Steam endpoint check")
    parser.add_argument("--expect", choices=["shared", "withheld", "signed-out"], default="shared")
    parser.add_argument("--every", type=float, default=5)
    args = parser.parse_args()
    {"pair": pair, "check": check, "serverinfo": serverinfo, "watch": watch}[args.command](args)
    if failures:
        print(f"\n{len(failures)} check(s) failed.")
        sys.exit(1)
    if args.command == "check":
        print("\nAll checks passed.")


if __name__ == "__main__":
    main()
