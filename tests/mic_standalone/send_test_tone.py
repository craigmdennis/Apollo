"""Pair with Apollo as a remote microphone and send Opus silence for 20 seconds.

Usage: python3 send_test_tone.py <host-address> [config-port]
Requires: pip install cryptography requests
The PIN is printed. Enter it on the Microphone tab of the Apollo PIN page.

The host certificate is saved on the first run and pinned on every later request,
which is the same trust model that Calliope uses.
"""
import base64
import hashlib
import hmac
import json
import os
import secrets
import socket
import ssl
import struct
import sys
import time

import requests
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from requests.adapters import HTTPAdapter

host = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 47990
base = f"https://{host}:{port}"
here = os.path.dirname(os.path.abspath(__file__))
state_file = os.path.join(here, ".send_test_tone.json")
cert_file = os.path.join(here, ".send_test_tone.pem")

# A 20 ms mono Opus frame of silence: the three-byte DTX frame.
OPUS_SILENCE = bytes([0xF8, 0xFF, 0xFE])


class PinnedAdapter(HTTPAdapter):
    """Trust only the saved host certificate. The certificate is self-signed and
    carries no host name, so the host name check is replaced by the pin."""

    def init_poolmanager(self, *args, **kwargs):
        kwargs["assert_hostname"] = False
        return super().init_poolmanager(*args, **kwargs)


if not os.path.exists(cert_file):
    with open(cert_file, "w") as handle:
        handle.write(ssl.get_server_certificate((host, port)))
    print("Saved the host certificate. Later runs accept only this certificate.")

http = requests.Session()
http.mount("https://", PinnedAdapter())
http.verify = cert_file


def fingerprint():
    with open(cert_file) as handle:
        return hashlib.sha256(ssl.PEM_cert_to_DER_cert(handle.read())).digest()


def pair():
    pin = f"{secrets.randbelow(10000):04d}"
    nonce = secrets.token_bytes(16)
    proof = hmac.new(pin.encode(), fingerprint() + nonce, hashlib.sha256).digest()
    reply = http.post(f"{base}/api/mic/pair", json={
        "name": "Reference sender",
        "nonce": base64.b64encode(nonce).decode(),
        "proof": base64.b64encode(proof).decode(),
    }).json()
    print(f"PIN: {pin}  (enter it on the Microphone tab of the PIN page)")
    while True:
        time.sleep(2)
        status = http.post(f"{base}/api/mic/pair/status",
                           json={"request_id": reply["request_id"]}).json()
        if status["state"] == "paired":
            return status["token"]
        if status["state"] == "expired":
            sys.exit("Pairing expired")


def packet(key, ptype, session_id, sequence, payload):
    nonce = bytes([0, ptype, 0, 0]) + struct.pack(">II", session_id, sequence)
    sealed = AESGCM(key).encrypt(nonce, payload, None)
    return bytes([ptype]) + struct.pack(">II", session_id, sequence) + sealed[-16:] + sealed[:-16]


if os.path.exists(state_file):
    token = json.load(open(state_file))["token"]
else:
    token = pair()
    json.dump({"token": token}, open(state_file, "w"))

auth = {"Authorization": f"Bearer {token}"}
session = http.post(f"{base}/api/mic/session", headers=auth).json()
print("session:", {k: v for k, v in session.items() if k != "key"})
key = base64.b64decode(session["key"])
sid = session["session_id"]

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.settimeout(0.001)
audio_seq = ping_seq = 0
start = time.time()
while time.time() - start < 20:
    sock.sendto(packet(key, 0, sid, audio_seq, OPUS_SILENCE), (host, session["port"]))
    audio_seq += 1
    if audio_seq % 50 == 0:
        sock.sendto(packet(key, 1, sid, ping_seq, b""), (host, session["port"]))
        ping_seq += 1
        try:
            data, _ = sock.recvfrom(64)
            nonce = bytes([1, 2, 0, 0]) + data[1:9]
            code = AESGCM(key).decrypt(nonce, data[25:] + data[9:25], None)
            print("pong error code:", code[0])
        except socket.timeout:
            print("no pong")
    time.sleep(0.02)

http.delete(f"{base}/api/mic/session", headers=auth)
print("session ended")
