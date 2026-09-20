# Calliope host receiver implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Apollo on Windows receives encrypted microphone audio from a paired mic device and plays it into the Steam Streaming Microphone, with no change to streaming.

**Architecture:** Four platform-neutral units (packet protocol, jitter buffer, mic device store, pairing state) are built and tested first, on any machine. A Windows writer owns the render thread and pulls decoded audio through a callback, so the audio device clock is the only playout clock. An orchestrator in `src/mic.cpp` owns one thread, one UDP socket, and one mic session. Seven config server routes, four tray functions, and a Microphone tab on the PIN page connect the orchestrator to the outside.

**Tech stack:** C++23, OpenSSL (AES-128-GCM, HMAC-SHA256), Boost.Asio UDP, libopus, nlohmann/json, WASAPI, GoogleTest, Vue 3.

**Spec:** `docs/superpowers/specs/2026-09-20-calliope-mic-sidecar-design.md`. Design evidence for the Windows writer and for playout is in `docs/superpowers/research/`. This plan covers build-order items 1, 2, and the host half of item 5. The Calliope app has its own plan.

## Global constraints

- All work goes on the branch `feature/calliope-mic`.
- `src/stream.cpp`, `src/rtsp.cpp`, `src/nvhttp.cpp`, `src/audio.cpp`, and `src/video.cpp` receive no edits.
- Mic code never reads or writes `sunshine_state.json` or `named_devices`.
- Until a mic device is paired, Apollo opens no mic socket, installs no driver, and changes no audio device.
- Packet layout: type (1 byte), session id (4 bytes, big-endian), sequence (4 bytes, big-endian), tag (16 bytes), payload.
- Nonce layout, 12 bytes: direction, type, two zero bytes, session id, sequence. Direction is 0 toward the host and 1 toward Calliope.
- Each packet type has its own sequence counter, starting at 0.
- Audio payload: one Opus frame, 48 kHz, mono, 20 ms.
- UDP port: `net::map_port(13)`.
- Pairing limits: one pending request per source address, four in total, 120 second expiry, three wrong PINs cancel every pending request.
- Jitter buffer: 40 ms prebuffer (2 frames), 100 ms maximum (5 frames). While playing, an empty buffer conceals up to 5 frames, then returns to prebuffering.
- Playout is pull-based: the virtual microphone's render thread requests one decoded packet at a time. No timer drives playout.
- Both Steam Streaming Microphone endpoints are set to 2 channels, 32-bit PCM, 48000 Hz before the render client is initialised.
- A mic session ends after 5 seconds with no valid packet.
- Pong error codes: 0 none, 1 virtual microphone missing, 2 virtual microphone cannot be opened, 3 repeated decode failures, 4 replaced by another mic device.
- C++ follows `.clang-format` exactly: 2-space indent, no column limit, right-aligned pointers.
- No emoji in code, UI text, commit messages, or documentation.
- The UI strings in Task 8 are copied verbatim from the spec.
- The target build is Windows under MSYS2 UCRT64. The macOS checkout builds only the standalone test target and syntax checks.

### Verification on two machines

Two kinds of check appear in this plan.

- **Local check.** Runs on the macOS development machine. The standalone test target compiles the platform-neutral units with OpenSSL and GoogleTest. The syntax check compiles one Apollo source file with `-fsyntax-only`, using flags from the existing `build/compile_commands.json`.
- **Windows check.** Runs on the Windows PC, by the operator. Prefix each command with `C:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.

The Apollo contributing rules state that AI-generated tests are not trusted on their own. Each Windows check is therefore a manual verification step and is required.

## File structure

| File | Responsibility |
|---|---|
| `src/mic_protocol.h`, `.cpp` | Packet header, nonce, AES-GCM packet encrypt and decrypt, PIN proof, certificate fingerprint |
| `src/mic_jitter.h`, `.cpp` | Orders audio frames, reports lost frames, enforces prebuffer and maximum depth |
| `src/mic_store.h`, `.cpp` | Reads and writes `mic_state.json`, hashes and checks mic tokens |
| `src/mic_pairing.h`, `.cpp` | Pending pairing requests, limits, expiry, PIN check |
| `src/mic_playout.h`, `.cpp` | Jitter buffer plus Opus decoder for one mic session, with the decode-failure latch |
| `src/mic.h`, `.cpp` | Orchestrator: thread lifecycle, UDP socket, mic session, tray calls |
| `src/platform/windows/mic_write.cpp` | WASAPI render thread that pulls audio, endpoint format normalisation, driver install, default capture device |
| `src/platform/common.h` | `virtual_mic_t` interface and factory declarations |
| `src/platform/linux/audio.cpp`, `src/platform/macos/microphone.mm` | Factories that report "unsupported" |
| `src/confighttp.cpp` | Seven routes, LAN origin rule, allowlist entry |
| `src/system_tray.h`, `.cpp` | Four notification functions |
| `src/main.cpp` | `mic::start()` and `mic::stop()` |
| `src_assets/common/assets/web/pin.html` | Microphone tab and mic device list |
| `tests/unit/test_mic_*.cpp` | Unit tests, compiled by both the Apollo test suite and the standalone target |
| `tests/mic_standalone/` | Standalone CMake target and syntax check helper |
| `docs/remote_microphone.md` | User documentation |

The spec names `src/mic.cpp` as the home of all platform-neutral code. This plan splits the four pure units into their own files so that each one compiles and tests without the rest of Apollo.

---

### Task 1: Packet protocol and the standalone test target

**Files:**
- Create: `src/mic_protocol.h`
- Create: `src/mic_protocol.cpp`
- Create: `tests/unit/test_mic_protocol.cpp`
- Create: `tests/fixtures/mic_vectors.json`
- Create: `tests/mic_standalone/CMakeLists.txt`
- Create: `tests/mic_standalone/syntax_check.py`
- Modify: `cmake/compile_definitions/common.cmake:86-87`

**Interfaces:**
- Consumes: `crypto::cipher::gcm_t`, `crypto::aes_t`, `crypto::sha256_t`, `crypto::x509()` from `src/crypto.h`.
- Produces, in namespace `mic::protocol`:
  - `enum class packet_type_e : std::uint8_t { audio = 0, ping = 1, pong = 2 }`
  - `enum class direction_e : std::uint8_t { to_host = 0, to_client = 1 }`
  - `enum class error_e : std::uint8_t { none = 0, device_missing = 1, device_open_failed = 2, decode_failed = 3, replaced = 4 }`
  - `struct header_t { packet_type_e type; std::uint32_t session_id; std::uint32_t sequence; }`
  - `struct packet_t { header_t header; std::vector<std::uint8_t> payload; }`
  - `crypto::aes_t build_nonce(direction_e, const header_t &)`
  - `std::optional<header_t> parse_header(std::string_view datagram)`
  - `std::optional<packet_t> decrypt_packet(crypto::cipher::gcm_t &, direction_e, std::string_view datagram)`
  - `std::vector<std::uint8_t> encrypt_packet(crypto::cipher::gcm_t &, direction_e, const header_t &, std::string_view payload)`
  - `crypto::sha256_t pin_proof(std::string_view pin, const crypto::sha256_t &fingerprint, std::string_view nonce)`
  - `std::optional<crypto::sha256_t> cert_fingerprint(std::string_view pem)`

The byte strings in `tests/fixtures/mic_vectors.json` were produced by an independent implementation (Python `cryptography`, AES-GCM, and `hmac`). The file is the shared test vector file from the spec. The Calliope plan copies it unchanged, so both implementations are checked against the same bytes.

- [ ] **Step 1: Create the standalone test target**

Create `tests/mic_standalone/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(test_mic_standalone CXX)

# Builds the platform-neutral mic units and their tests without the rest of Apollo.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(APOLLO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")

find_package(OpenSSL REQUIRED)

include(FetchContent)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

set(INSTALL_GTEST OFF)
add_subdirectory("${APOLLO_DIR}/third-party/googletest" googletest)

set(MIC_SOURCES "${APOLLO_DIR}/src/crypto.cpp")
foreach(unit mic_protocol mic_jitter mic_store mic_pairing)
    if(EXISTS "${APOLLO_DIR}/src/${unit}.cpp")
        list(APPEND MIC_SOURCES "${APOLLO_DIR}/src/${unit}.cpp")
    endif()
endforeach()

file(GLOB MIC_TEST_SOURCES CONFIGURE_DEPENDS "${APOLLO_DIR}/tests/unit/test_mic_*.cpp")

add_executable(${PROJECT_NAME} ${MIC_SOURCES} ${MIC_TEST_SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE "${APOLLO_DIR}")
# The Apollo test suite defines SUNSHINE_SOURCE_DIR. The mic tests use it to find tests/fixtures.
target_compile_definitions(${PROJECT_NAME} PRIVATE SUNSHINE_SOURCE_DIR="${APOLLO_DIR}")
target_link_libraries(${PROJECT_NAME} PRIVATE
        OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json gtest gtest_main)
```

- [ ] **Step 2: Create the syntax check helper**

Create `tests/mic_standalone/syntax_check.py`:

```python
"""Syntax-check one Apollo source file on a machine that cannot link Apollo.

Usage: python3 tests/mic_standalone/syntax_check.py src/mic.cpp
Requires build/compile_commands.json from an earlier CMake configure.
"""
import json
import os
import shlex
import subprocess
import sys

root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
target = os.path.abspath(sys.argv[1])
with open(os.path.join(root, "build", "compile_commands.json")) as handle:
    entries = json.load(handle)
entry = next(e for e in entries if e["file"].endswith("src/confighttp.cpp"))

args = []
skip = False
for arg in shlex.split(entry["command"]):
    if skip:
        skip = False
        continue
    if arg in ("-o", "-MF", "-MT"):
        skip = True
        continue
    if arg in ("-c", "-MD") or arg.endswith("confighttp.cpp"):
        continue
    args.append(arg)

args += [
    "-I/opt/homebrew/opt/openssl@3/include",
    "-I/opt/homebrew/include",
    "-fsyntax-only",
    target,
]
sys.exit(subprocess.call(args, cwd=entry["directory"]))
```

- [ ] **Step 3: Write the shared test vectors and the failing tests**

Create `tests/fixtures/mic_vectors.json`:

```json
{
  "key": "000102030405060708090a0b0c0d0e0f",
  "session_id": 16909060,
  "packets": [
    {
      "name": "audio",
      "type": 0,
      "direction": 0,
      "sequence": 7,
      "payload": "6f7075732d6672616d652d6279746573",
      "nonce": "000000000102030400000007",
      "datagram": "000102030400000007e3f3ed704230288352a29d4e3c89e491d6321feab5073430ab02084b184f4096"
    },
    {
      "name": "ping",
      "type": 1,
      "direction": 0,
      "sequence": 8,
      "payload": "",
      "nonce": "000100000102030400000008",
      "datagram": "0101020304000000080fbf941399f5707ebb96329105f94aed"
    },
    {
      "name": "pong",
      "type": 2,
      "direction": 1,
      "sequence": 8,
      "payload": "01",
      "nonce": "010200000102030400000008",
      "datagram": "020102030400000008596ddb60fbaec3440d09f63c28cc910cce"
    }
  ],
  "pin_proof": {
    "pin": "4821",
    "fingerprint": "69be57455b3b4f84c7c23140e875002791c5a5509ca9d0c644a63d5eaf836cce",
    "nonce": "101112131415161718191a1b1c1d1e1f",
    "proof": "a0aa0bf1c951d16d05f3ba31e554f10be50d504a4e2f786cb10bae190ebd1448"
  }
}
```

Create `tests/unit/test_mic_protocol.cpp`:

```cpp
/**
 * @file tests/unit/test_mic_protocol.cpp
 * @brief Test src/mic_protocol.* against the shared vectors in tests/fixtures/mic_vectors.json.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

#include <src/mic_protocol.h>

namespace {
  using namespace mic::protocol;

  std::string from_hex(std::string_view hex) {
    std::string out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
      out.push_back(static_cast<char>(std::stoi(std::string {hex.substr(i, 2)}, nullptr, 16)));
    }
    return out;
  }

  template<class T>
  std::string to_hex(const T &container) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (auto value : container) {
      auto byte = static_cast<unsigned char>(value);
      out.push_back(digits[byte >> 4]);
      out.push_back(digits[byte & 0x0F]);
    }
    return out;
  }

  const nlohmann::json &vectors() {
    static const nlohmann::json tree = []() {
      std::ifstream in {std::string {SUNSHINE_SOURCE_DIR} + "/tests/fixtures/mic_vectors.json"};
      return nlohmann::json::parse(in);
    }();
    return tree;
  }

  crypto::cipher::gcm_t vector_cipher() {
    auto key_bytes = from_hex(vectors()["key"].get<std::string>());
    return crypto::cipher::gcm_t {crypto::aes_t {key_bytes.begin(), key_bytes.end()}, false};
  }

  header_t header_of(const nlohmann::json &packet) {
    return {
      static_cast<packet_type_e>(packet["type"].get<int>()),
      vectors()["session_id"].get<std::uint32_t>(),
      packet["sequence"].get<std::uint32_t>()
    };
  }

  direction_e direction_of(const nlohmann::json &packet) {
    return static_cast<direction_e>(packet["direction"].get<int>());
  }

  std::string audio_datagram() {
    return from_hex(vectors()["packets"][0]["datagram"].get<std::string>());
  }
}  // namespace

TEST(MicProtocol, VectorNonces) {
  ASSERT_EQ(vectors()["packets"].size(), 3u);
  for (const auto &packet : vectors()["packets"]) {
    EXPECT_EQ(to_hex(build_nonce(direction_of(packet), header_of(packet))), packet["nonce"].get<std::string>()) << packet["name"];
  }
}

TEST(MicProtocol, VectorsDecrypt) {
  auto cipher = vector_cipher();
  for (const auto &packet : vectors()["packets"]) {
    auto result = decrypt_packet(cipher, direction_of(packet), from_hex(packet["datagram"].get<std::string>()));
    ASSERT_TRUE(result.has_value()) << packet["name"];
    EXPECT_EQ(result->header.type, header_of(packet).type) << packet["name"];
    EXPECT_EQ(result->header.session_id, header_of(packet).session_id) << packet["name"];
    EXPECT_EQ(result->header.sequence, header_of(packet).sequence) << packet["name"];
    EXPECT_EQ(to_hex(result->payload), packet["payload"].get<std::string>()) << packet["name"];
  }
}

TEST(MicProtocol, VectorsEncrypt) {
  auto cipher = vector_cipher();
  for (const auto &packet : vectors()["packets"]) {
    auto datagram = encrypt_packet(cipher, direction_of(packet), header_of(packet), from_hex(packet["payload"].get<std::string>()));
    EXPECT_EQ(to_hex(datagram), packet["datagram"].get<std::string>()) << packet["name"];
  }
}

TEST(MicProtocol, RejectsTamperedSequence) {
  auto cipher = vector_cipher();
  auto datagram = audio_datagram();
  datagram[8] ^= 0x01;  // last byte of the sequence field
  EXPECT_FALSE(decrypt_packet(cipher, direction_e::to_host, datagram).has_value());
}

TEST(MicProtocol, RejectsWrongDirection) {
  auto cipher = vector_cipher();
  EXPECT_FALSE(decrypt_packet(cipher, direction_e::to_client, audio_datagram()).has_value());
}

TEST(MicProtocol, RejectsShortDatagram) {
  EXPECT_FALSE(parse_header(std::string(24, '\0')).has_value());
}

TEST(MicProtocol, RejectsUnknownType) {
  std::string datagram(25, '\0');
  datagram[0] = 3;
  EXPECT_FALSE(parse_header(datagram).has_value());
}

TEST(MicProtocol, RejectsOversizedDatagram) {
  EXPECT_FALSE(parse_header(std::string(9 + 16 + 1401, '\0')).has_value());
}

TEST(MicProtocol, VectorPinProof) {
  const auto &vector = vectors()["pin_proof"];
  auto fingerprint_bytes = from_hex(vector["fingerprint"].get<std::string>());
  crypto::sha256_t fingerprint;
  std::copy(fingerprint_bytes.begin(), fingerprint_bytes.end(), fingerprint.begin());

  auto proof = pin_proof(vector["pin"].get<std::string>(), fingerprint, from_hex(vector["nonce"].get<std::string>()));
  EXPECT_EQ(to_hex(proof), vector["proof"].get<std::string>());
}

TEST(MicProtocol, PinProofChangesWithPin) {
  auto fingerprint = crypto::hash("test-certificate-der");
  std::string nonce(16, 'n');
  EXPECT_NE(pin_proof("4821", fingerprint, nonce), pin_proof("4822", fingerprint, nonce));
}

TEST(MicProtocol, CertFingerprintIsStableAndDistinct) {
  auto first = crypto::gen_creds("mic-test-one", 2048);
  auto second = crypto::gen_creds("mic-test-two", 2048);
  auto a = cert_fingerprint(first.x509);
  auto b = cert_fingerprint(first.x509);
  auto c = cert_fingerprint(second.x509);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(*a, *b);
  EXPECT_NE(*a, *c);
}

TEST(MicProtocol, CertFingerprintRejectsGarbage) {
  EXPECT_FALSE(cert_fingerprint("not a certificate").has_value());
}
```

- [ ] **Step 4: Create the header so the tests compile and fail at link time**

Create `src/mic_protocol.h`:

```cpp
/**
 * @file src/mic_protocol.h
 * @brief Wire protocol for the remote microphone: packets, nonces, and the PIN proof.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// local includes
#include "crypto.h"

namespace mic::protocol {
  enum class packet_type_e : std::uint8_t {
    audio = 0,  ///< One Opus frame
    ping = 1,  ///< Keepalive from Calliope
    pong = 2  ///< Reply from the host, carries one error_e byte
  };

  enum class direction_e : std::uint8_t {
    to_host = 0,
    to_client = 1
  };

  enum class error_e : std::uint8_t {
    none = 0,
    device_missing = 1,
    device_open_failed = 2,
    decode_failed = 3,
    replaced = 4
  };

  constexpr std::size_t HEADER_SIZE = 9;
  constexpr std::size_t TAG_SIZE = crypto::cipher::tag_size;
  constexpr std::size_t MAX_PAYLOAD = 1400;

  struct header_t {
    packet_type_e type;
    std::uint32_t session_id;
    std::uint32_t sequence;
  };

  struct packet_t {
    header_t header;
    std::vector<std::uint8_t> payload;
  };

  /**
   * @brief Build the 12-byte GCM nonce: direction, type, two zero bytes, session id, sequence.
   */
  crypto::aes_t build_nonce(direction_e direction, const header_t &header);

  /**
   * @brief Read the clear header. Returns nothing for a short, oversized, or unknown-type datagram.
   */
  std::optional<header_t> parse_header(std::string_view datagram);

  /**
   * @brief Authenticate and decrypt a datagram laid out as [header][tag][ciphertext].
   */
  std::optional<packet_t> decrypt_packet(crypto::cipher::gcm_t &cipher, direction_e direction, std::string_view datagram);

  /**
   * @brief Encrypt a payload. Returns an empty vector when encryption fails.
   */
  std::vector<std::uint8_t> encrypt_packet(crypto::cipher::gcm_t &cipher, direction_e direction, const header_t &header, std::string_view payload);

  /**
   * @brief HMAC-SHA256 with the PIN as key over fingerprint || nonce.
   */
  crypto::sha256_t pin_proof(std::string_view pin, const crypto::sha256_t &fingerprint, std::string_view nonce);

  /**
   * @brief SHA-256 of the DER form of a PEM certificate.
   */
  std::optional<crypto::sha256_t> cert_fingerprint(std::string_view pem);
}  // namespace mic::protocol
```

- [ ] **Step 5: Run the tests and confirm they fail**

Run:

```bash
cmake -S tests/mic_standalone -B build/mic_standalone -G Ninja -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
ninja -C build/mic_standalone
```

Expected: the build FAILS at link time with undefined symbols such as `mic::protocol::build_nonce`.

- [ ] **Step 6: Write the implementation**

Create `src/mic_protocol.cpp`:

```cpp
/**
 * @file src/mic_protocol.cpp
 * @brief Definitions for the remote microphone wire protocol.
 */
// standard includes
#include <algorithm>
#include <string>

// lib includes
#include <openssl/hmac.h>
#include <openssl/x509.h>

// local includes
#include "mic_protocol.h"

namespace mic::protocol {
  namespace {
    void put_be32(std::uint8_t *out, std::uint32_t value) {
      out[0] = static_cast<std::uint8_t>(value >> 24);
      out[1] = static_cast<std::uint8_t>(value >> 16);
      out[2] = static_cast<std::uint8_t>(value >> 8);
      out[3] = static_cast<std::uint8_t>(value);
    }

    std::uint32_t get_be32(const char *in) {
      auto byte = [in](int index) {
        return static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[index]));
      };
      return (byte(0) << 24) | (byte(1) << 16) | (byte(2) << 8) | byte(3);
    }
  }  // namespace

  crypto::aes_t build_nonce(direction_e direction, const header_t &header) {
    crypto::aes_t nonce(12, 0);
    nonce[0] = static_cast<std::uint8_t>(direction);
    nonce[1] = static_cast<std::uint8_t>(header.type);
    put_be32(nonce.data() + 4, header.session_id);
    put_be32(nonce.data() + 8, header.sequence);
    return nonce;
  }

  std::optional<header_t> parse_header(std::string_view datagram) {
    if (datagram.size() < HEADER_SIZE + TAG_SIZE || datagram.size() > HEADER_SIZE + TAG_SIZE + MAX_PAYLOAD) {
      return std::nullopt;
    }

    auto type = static_cast<std::uint8_t>(datagram[0]);
    if (type > static_cast<std::uint8_t>(packet_type_e::pong)) {
      return std::nullopt;
    }

    return header_t {
      static_cast<packet_type_e>(type),
      get_be32(datagram.data() + 1),
      get_be32(datagram.data() + 5)
    };
  }

  std::optional<packet_t> decrypt_packet(crypto::cipher::gcm_t &cipher, direction_e direction, std::string_view datagram) {
    auto header = parse_header(datagram);
    if (!header) {
      return std::nullopt;
    }

    auto nonce = build_nonce(direction, *header);
    packet_t packet {*header, {}};
    // gcm_t reads [tag][ciphertext], which is the layout that follows the header.
    if (cipher.decrypt(datagram.substr(HEADER_SIZE), packet.payload, &nonce) != 0) {
      return std::nullopt;
    }
    return packet;
  }

  std::vector<std::uint8_t> encrypt_packet(crypto::cipher::gcm_t &cipher, direction_e direction, const header_t &header, std::string_view payload) {
    auto nonce = build_nonce(direction, header);
    std::vector<std::uint8_t> out(HEADER_SIZE + TAG_SIZE + crypto::cipher::round_to_pkcs7_padded(payload.size()));
    out[0] = static_cast<std::uint8_t>(header.type);
    put_be32(out.data() + 1, header.session_id);
    put_be32(out.data() + 5, header.sequence);

    // gcm_t writes [tag][ciphertext] at the given address.
    auto length = cipher.encrypt(payload, out.data() + HEADER_SIZE, &nonce);
    if (length < 0) {
      return {};
    }
    out.resize(HEADER_SIZE + TAG_SIZE + static_cast<std::size_t>(length));
    return out;
  }

  crypto::sha256_t pin_proof(std::string_view pin, const crypto::sha256_t &fingerprint, std::string_view nonce) {
    std::string message {reinterpret_cast<const char *>(fingerprint.data()), fingerprint.size()};
    message.append(nonce);

    crypto::sha256_t proof {};
    unsigned int length = 0;
    HMAC(EVP_sha256(), pin.data(), static_cast<int>(pin.size()), reinterpret_cast<const unsigned char *>(message.data()), message.size(), proof.data(), &length);
    return proof;
  }

  std::optional<crypto::sha256_t> cert_fingerprint(std::string_view pem) {
    auto cert = crypto::x509(pem);
    if (!cert) {
      return std::nullopt;
    }

    crypto::sha256_t fingerprint {};
    unsigned int length = 0;
    if (X509_digest(cert.get(), EVP_sha256(), fingerprint.data(), &length) != 1) {
      return std::nullopt;
    }
    return fingerprint;
  }
}  // namespace mic::protocol
```

- [ ] **Step 7: Run the tests and confirm they pass**

Run:

```bash
ninja -C build/mic_standalone && ./build/mic_standalone/test_mic_standalone --gtest_filter='MicProtocol.*'
```

Expected: 12 tests, all PASS.

- [ ] **Step 8: Add the sources to the Apollo build**

In `cmake/compile_definitions/common.cmake`, after the line `"${CMAKE_SOURCE_DIR}/src/confighttp.h"`, add:

```cmake
        "${CMAKE_SOURCE_DIR}/src/mic_protocol.cpp"
        "${CMAKE_SOURCE_DIR}/src/mic_protocol.h"
```

The Apollo test suite finds `tests/unit/test_mic_protocol.cpp` through its existing `GLOB_RECURSE`.

- [ ] **Step 9: Commit**

```bash
git add src/mic_protocol.h src/mic_protocol.cpp tests/unit/test_mic_protocol.cpp tests/fixtures/mic_vectors.json tests/mic_standalone cmake/compile_definitions/common.cmake
git commit -m "feat(mic): add remote microphone packet protocol"
```

---

### Task 2: Jitter buffer

**Files:**
- Create: `src/mic_jitter.h`
- Create: `src/mic_jitter.cpp`
- Create: `tests/unit/test_mic_jitter.cpp`
- Modify: `cmake/compile_definitions/common.cmake` (after the `mic_protocol` lines)

**Interfaces:**
- Consumes: nothing.
- Produces, in namespace `mic`:
  - `class jitter_buffer_t` with `enum class pop_e { wait, frame, lost }`
  - `struct jitter_buffer_t::pop_result_t { pop_e kind; std::vector<std::uint8_t> payload; }`
  - `bool push(std::uint32_t sequence, std::vector<std::uint8_t> payload)`. Returns false for a duplicate or late frame.
  - `pop_result_t pop()`. Called once per 20 ms.
  - `std::size_t size() const`
  - `static constexpr std::size_t PREBUFFER_FRAMES = 2`, `MAX_FRAMES = 10`

Behaviour: playout starts when 2 frames are queued. A missing sequence returns `lost`, and the caller runs Opus loss concealment. An empty buffer stops playout and returns `wait`, which covers the muted state where Calliope sends no audio.

- [ ] **Step 1: Write the failing tests**

Create `tests/unit/test_mic_jitter.cpp`:

```cpp
/**
 * @file tests/unit/test_mic_jitter.cpp
 * @brief Test src/mic_jitter.*.
 */
#include <gtest/gtest.h>

#include <src/mic_jitter.h>

namespace {
  using mic::jitter_buffer_t;
  using pop_e = mic::jitter_buffer_t::pop_e;

  std::vector<std::uint8_t> frame(std::uint8_t marker) {
    return {marker};
  }
}  // namespace

TEST(MicJitter, WaitsUntilPrebufferIsFull) {
  jitter_buffer_t buffer;
  EXPECT_EQ(buffer.pop().kind, pop_e::wait);
  buffer.push(0, frame(0));
  EXPECT_EQ(buffer.pop().kind, pop_e::wait);
  buffer.push(1, frame(1));
  EXPECT_EQ(buffer.pop().kind, pop_e::frame);
}

TEST(MicJitter, PlaysInSequenceOrder) {
  jitter_buffer_t buffer;
  buffer.push(1, frame(1));
  buffer.push(0, frame(0));
  auto first = buffer.pop();
  auto second = buffer.pop();
  ASSERT_EQ(first.kind, pop_e::frame);
  ASSERT_EQ(second.kind, pop_e::frame);
  EXPECT_EQ(first.payload, frame(0));
  EXPECT_EQ(second.payload, frame(1));
}

TEST(MicJitter, StartsAtFirstQueuedSequence) {
  jitter_buffer_t buffer;
  buffer.push(500, frame(5));
  buffer.push(501, frame(6));
  auto first = buffer.pop();
  ASSERT_EQ(first.kind, pop_e::frame);
  EXPECT_EQ(first.payload, frame(5));
}

TEST(MicJitter, ReportsMissingFrameAsLost) {
  jitter_buffer_t buffer;
  buffer.push(0, frame(0));
  buffer.push(2, frame(2));
  EXPECT_EQ(buffer.pop().kind, pop_e::frame);
  EXPECT_EQ(buffer.pop().kind, pop_e::lost);
  auto third = buffer.pop();
  ASSERT_EQ(third.kind, pop_e::frame);
  EXPECT_EQ(third.payload, frame(2));
}

TEST(MicJitter, RejectsDuplicate) {
  jitter_buffer_t buffer;
  EXPECT_TRUE(buffer.push(0, frame(0)));
  EXPECT_FALSE(buffer.push(0, frame(9)));
  EXPECT_EQ(buffer.size(), 1u);
}

TEST(MicJitter, RejectsLateFrame) {
  jitter_buffer_t buffer;
  buffer.push(0, frame(0));
  buffer.push(1, frame(1));
  buffer.push(2, frame(2));
  buffer.pop();
  buffer.pop();
  EXPECT_FALSE(buffer.push(0, frame(0)));
}

TEST(MicJitter, DropsOldestAboveMaximum) {
  jitter_buffer_t buffer;
  for (std::uint32_t sequence = 0; sequence < 12; ++sequence) {
    buffer.push(sequence, frame(static_cast<std::uint8_t>(sequence)));
  }
  EXPECT_EQ(buffer.size(), jitter_buffer_t::MAX_FRAMES);
  auto first = buffer.pop();
  ASSERT_EQ(first.kind, pop_e::frame);
  EXPECT_EQ(first.payload, frame(2));
}

TEST(MicJitter, EmptyBufferRestartsPrebuffer) {
  jitter_buffer_t buffer;
  buffer.push(0, frame(0));
  buffer.push(1, frame(1));
  buffer.pop();
  buffer.pop();
  EXPECT_EQ(buffer.pop().kind, pop_e::wait);
  buffer.push(40, frame(4));
  EXPECT_EQ(buffer.pop().kind, pop_e::wait);
  buffer.push(41, frame(5));
  auto next = buffer.pop();
  ASSERT_EQ(next.kind, pop_e::frame);
  EXPECT_EQ(next.payload, frame(4));
}

TEST(MicJitter, SkipsALargeGap) {
  jitter_buffer_t buffer;
  buffer.push(0, frame(0));
  buffer.push(1, frame(1));
  buffer.pop();
  buffer.push(100, frame(7));
  buffer.pop();
  auto next = buffer.pop();
  ASSERT_EQ(next.kind, pop_e::frame);
  EXPECT_EQ(next.payload, frame(7));
}
```

- [ ] **Step 2: Create the header**

Create `src/mic_jitter.h`:

```cpp
/**
 * @file src/mic_jitter.h
 * @brief Orders remote microphone frames by sequence and reports lost frames.
 */
#pragma once

// standard includes
#include <cstdint>
#include <map>
#include <vector>

namespace mic {
  class jitter_buffer_t {
  public:
    enum class pop_e {
      wait,  ///< Nothing to play: prebuffering, or the buffer is empty
      frame,  ///< payload holds the next Opus frame
      lost  ///< The next sequence is missing. Run loss concealment.
    };

    struct pop_result_t {
      pop_e kind;
      std::vector<std::uint8_t> payload;
    };

    static constexpr std::size_t PREBUFFER_FRAMES = 2;  // 40 ms
    static constexpr std::size_t MAX_FRAMES = 10;  // 200 ms

    /**
     * @brief Queue one frame.
     * @return false for a duplicate, or for a frame older than the playout point.
     */
    bool push(std::uint32_t sequence, std::vector<std::uint8_t> payload);

    /**
     * @brief Take the next frame. Call once per 20 ms.
     */
    pop_result_t pop();

    std::size_t size() const {
      return frames.size();
    }

  private:
    std::map<std::uint32_t, std::vector<std::uint8_t>> frames;
    bool started = false;
    std::uint32_t next_sequence = 0;
  };
}  // namespace mic
```

- [ ] **Step 3: Run the tests and confirm they fail**

Run:

```bash
cmake -S tests/mic_standalone -B build/mic_standalone && ninja -C build/mic_standalone
```

Expected: the build FAILS at link time with undefined `mic::jitter_buffer_t::push`.

- [ ] **Step 4: Write the implementation**

Create `src/mic_jitter.cpp`:

```cpp
/**
 * @file src/mic_jitter.cpp
 * @brief Definitions for the remote microphone jitter buffer.
 */
#include "mic_jitter.h"

namespace mic {
  bool jitter_buffer_t::push(std::uint32_t sequence, std::vector<std::uint8_t> payload) {
    if (started && sequence < next_sequence) {
      return false;
    }
    if (!frames.emplace(sequence, std::move(payload)).second) {
      return false;
    }

    while (frames.size() > MAX_FRAMES) {
      frames.erase(frames.begin());
    }
    return true;
  }

  jitter_buffer_t::pop_result_t jitter_buffer_t::pop() {
    if (!started) {
      if (frames.size() < PREBUFFER_FRAMES) {
        return {pop_e::wait, {}};
      }
      started = true;
      next_sequence = frames.begin()->first;
    }

    if (frames.empty()) {
      started = false;
      return {pop_e::wait, {}};
    }

    auto first = frames.begin()->first;
    if (first > next_sequence && (first - next_sequence > MAX_FRAMES || frames.size() == MAX_FRAMES)) {
      // Either the sender moved far ahead, or dropping the oldest frames moved the queue past
      // the playout point. Concealing every missing frame would add delay, so skip to the queue.
      next_sequence = first;
    }

    auto it = frames.find(next_sequence);
    ++next_sequence;
    if (it == frames.end()) {
      return {pop_e::lost, {}};
    }

    pop_result_t result {pop_e::frame, std::move(it->second)};
    frames.erase(it);
    return result;
  }

}  // namespace mic
```

- [ ] **Step 5: Run the tests and confirm they pass**

Run:

```bash
ninja -C build/mic_standalone && ./build/mic_standalone/test_mic_standalone --gtest_filter='MicJitter.*'
```

Expected: 9 tests, all PASS.

- [ ] **Step 6: Add the sources to the Apollo build and commit**

In `cmake/compile_definitions/common.cmake`, after the `mic_protocol.h` line, add:

```cmake
        "${CMAKE_SOURCE_DIR}/src/mic_jitter.cpp"
        "${CMAKE_SOURCE_DIR}/src/mic_jitter.h"
```

```bash
git add src/mic_jitter.h src/mic_jitter.cpp tests/unit/test_mic_jitter.cpp cmake/compile_definitions/common.cmake
git commit -m "feat(mic): add jitter buffer for remote microphone frames"
```

---

### Task 3: Mic device store

**Files:**
- Create: `src/mic_store.h`
- Create: `src/mic_store.cpp`
- Create: `tests/unit/test_mic_store.cpp`
- Modify: `cmake/compile_definitions/common.cmake` (after the `mic_jitter` lines)

**Interfaces:**
- Consumes: `crypto::hash()` and `util::hex()` from `src/crypto.h` and `src/utility.h`.
- Produces, in namespace `mic`:
  - `struct device_t { std::string name; std::string uuid; std::string token_hash; }`
  - `class store_t` with `explicit store_t(std::filesystem::path file)`
  - `bool load()`. A missing file loads as empty and returns true. A corrupt file returns false.
  - `bool save() const`
  - `const std::vector<device_t> &devices() const`
  - `const device_t &add(std::string name, std::string uuid, const std::string &token)`
  - `bool remove(const std::string &uuid)`
  - `std::optional<device_t> authorize(const std::string &token) const`
  - `std::string previous_default_capture` (public field)
  - `static std::string hash_token(const std::string &token)`

The hash is unsalted SHA-256 in hex, the same construction as `http::hash_api_token`. A random 48-character token makes a salt unnecessary.

- [ ] **Step 1: Write the failing tests**

Create `tests/unit/test_mic_store.cpp`:

```cpp
/**
 * @file tests/unit/test_mic_store.cpp
 * @brief Test src/mic_store.*.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <src/mic_store.h>

namespace {
  class MicStore: public testing::Test {
  protected:
    void SetUp() override {
      file = std::filesystem::temp_directory_path() / ("mic_state_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".json");
      std::filesystem::remove(file);
    }

    void TearDown() override {
      std::filesystem::remove(file);
    }

    std::string file_text() const {
      std::ifstream in {file};
      std::stringstream text;
      text << in.rdbuf();
      return text.str();
    }

    std::filesystem::path file;
  };
}  // namespace

TEST_F(MicStore, MissingFileLoadsAsEmpty) {
  mic::store_t store {file};
  EXPECT_TRUE(store.load());
  EXPECT_TRUE(store.devices().empty());
}

TEST_F(MicStore, RoundTripsDevicesAndPreviousDefault) {
  mic::store_t store {file};
  store.add("Living room iPhone", "uuid-1", "token-one");
  store.previous_default_capture = "{device-id}";
  ASSERT_TRUE(store.save());

  mic::store_t reloaded {file};
  ASSERT_TRUE(reloaded.load());
  ASSERT_EQ(reloaded.devices().size(), 1u);
  EXPECT_EQ(reloaded.devices()[0].name, "Living room iPhone");
  EXPECT_EQ(reloaded.devices()[0].uuid, "uuid-1");
  EXPECT_EQ(reloaded.previous_default_capture, "{device-id}");
}

TEST_F(MicStore, AuthorizesOnlyTheMatchingToken) {
  mic::store_t store {file};
  store.add("MacBook Pro", "uuid-2", "token-two");
  auto device = store.authorize("token-two");
  ASSERT_TRUE(device.has_value());
  EXPECT_EQ(device->uuid, "uuid-2");
  EXPECT_FALSE(store.authorize("token-three").has_value());
  EXPECT_FALSE(store.authorize("").has_value());
}

TEST_F(MicStore, NeverWritesThePlainToken) {
  mic::store_t store {file};
  store.add("MacBook Pro", "uuid-2", "plain-secret-token");
  ASSERT_TRUE(store.save());
  EXPECT_EQ(file_text().find("plain-secret-token"), std::string::npos);
  EXPECT_NE(file_text().find(mic::store_t::hash_token("plain-secret-token")), std::string::npos);
}

TEST_F(MicStore, RemovesByUuid) {
  mic::store_t store {file};
  store.add("A", "uuid-a", "token-a");
  store.add("B", "uuid-b", "token-b");
  EXPECT_TRUE(store.remove("uuid-a"));
  EXPECT_FALSE(store.remove("uuid-a"));
  ASSERT_EQ(store.devices().size(), 1u);
  EXPECT_EQ(store.devices()[0].uuid, "uuid-b");
  EXPECT_FALSE(store.authorize("token-a").has_value());
}

TEST_F(MicStore, CorruptFileReturnsFalseAndStaysEmpty) {
  {
    std::ofstream out {file};
    out << "{ not json";
  }
  mic::store_t store {file};
  EXPECT_FALSE(store.load());
  EXPECT_TRUE(store.devices().empty());
}
```

- [ ] **Step 2: Create the header**

Create `src/mic_store.h`:

```cpp
/**
 * @file src/mic_store.h
 * @brief Persistent list of paired remote microphone devices (mic_state.json).
 */
#pragma once

// standard includes
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mic {
  struct device_t {
    std::string name;
    std::string uuid;
    std::string token_hash;  ///< Unsalted SHA-256 of the mic token, in hex
  };

  /**
   * @brief Stores mic devices in their own file. Never touches sunshine_state.json.
   */
  class store_t {
  public:
    explicit store_t(std::filesystem::path file);

    bool load();
    bool save() const;

    const std::vector<device_t> &devices() const {
      return _devices;
    }

    const device_t &add(std::string name, std::string uuid, const std::string &token);
    bool remove(const std::string &uuid);
    std::optional<device_t> authorize(const std::string &token) const;

    static std::string hash_token(const std::string &token);

    /// Default capture device id saved before a switch. Non-empty after a crash during a mic session.
    std::string previous_default_capture;

  private:
    std::filesystem::path _file;
    std::vector<device_t> _devices;
  };
}  // namespace mic
```

- [ ] **Step 3: Run the tests and confirm they fail**

Run:

```bash
cmake -S tests/mic_standalone -B build/mic_standalone && ninja -C build/mic_standalone
```

Expected: the build FAILS at link time with undefined `mic::store_t::store_t`.

- [ ] **Step 4: Write the implementation**

Create `src/mic_store.cpp`:

```cpp
/**
 * @file src/mic_store.cpp
 * @brief Definitions for the remote microphone device store.
 */
// standard includes
#include <algorithm>
#include <fstream>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "crypto.h"
#include "mic_store.h"

namespace mic {
  store_t::store_t(std::filesystem::path file):
      _file {std::move(file)} {
  }

  bool store_t::load() {
    _devices.clear();
    previous_default_capture.clear();

    if (!std::filesystem::exists(_file)) {
      return true;
    }

    try {
      std::ifstream in {_file};
      auto tree = nlohmann::json::parse(in);
      for (const auto &node : tree.value("devices", nlohmann::json::array())) {
        _devices.push_back({node.value("name", ""), node.value("uuid", ""), node.value("token_hash", "")});
      }
      previous_default_capture = tree.value("previous_default_capture", "");
      return true;
    } catch (const std::exception &) {
      _devices.clear();
      previous_default_capture.clear();
      return false;
    }
  }

  bool store_t::save() const {
    nlohmann::json tree;
    tree["devices"] = nlohmann::json::array();
    for (const auto &device : _devices) {
      tree["devices"].push_back({{"name", device.name}, {"uuid", device.uuid}, {"token_hash", device.token_hash}});
    }
    tree["previous_default_capture"] = previous_default_capture;

    // Write a sibling file and rename it, so a crash never leaves a half-written store.
    auto temp = _file;
    temp += ".tmp";
    {
      std::ofstream out {temp, std::ios::trunc};
      if (!out) {
        return false;
      }
      out << tree.dump(2);
      if (!out) {
        return false;
      }
    }

    std::error_code ec;
    std::filesystem::rename(temp, _file, ec);
    return !ec;
  }

  const device_t &store_t::add(std::string name, std::string uuid, const std::string &token) {
    _devices.push_back({std::move(name), std::move(uuid), hash_token(token)});
    return _devices.back();
  }

  bool store_t::remove(const std::string &uuid) {
    return std::erase_if(_devices, [&](const device_t &device) {
             return device.uuid == uuid;
           }) > 0;
  }

  std::optional<device_t> store_t::authorize(const std::string &token) const {
    if (token.empty()) {
      return std::nullopt;
    }
    auto hash = hash_token(token);
    auto it = std::find_if(_devices.begin(), _devices.end(), [&](const device_t &device) {
      return device.token_hash == hash;
    });
    if (it == _devices.end()) {
      return std::nullopt;
    }
    return *it;
  }

  std::string store_t::hash_token(const std::string &token) {
    return util::hex(crypto::hash(token)).to_string();
  }
}  // namespace mic
```

- [ ] **Step 5: Run the tests and confirm they pass**

Run:

```bash
ninja -C build/mic_standalone && ./build/mic_standalone/test_mic_standalone --gtest_filter='MicStore.*'
```

Expected: 6 tests, all PASS.

- [ ] **Step 6: Add the sources to the Apollo build and commit**

In `cmake/compile_definitions/common.cmake`, after the `mic_jitter.h` line, add:

```cmake
        "${CMAKE_SOURCE_DIR}/src/mic_store.cpp"
        "${CMAKE_SOURCE_DIR}/src/mic_store.h"
```

```bash
git add src/mic_store.h src/mic_store.cpp tests/unit/test_mic_store.cpp cmake/compile_definitions/common.cmake
git commit -m "feat(mic): add mic device store in mic_state.json"
```

---

### Task 4: Pairing state

**Files:**
- Create: `src/mic_pairing.h`
- Create: `src/mic_pairing.cpp`
- Create: `tests/unit/test_mic_pairing.cpp`
- Modify: `cmake/compile_definitions/common.cmake` (after the `mic_store` lines)

**Interfaces:**
- Consumes: `mic::protocol::pin_proof` from Task 1. `crypto::rand()` and `util::hex_vec()`.
- Produces, in namespace `mic`, `class pairing_t`:
  - `using clock_t = std::chrono::steady_clock`
  - `enum class state_e { pending, paired, expired }`
  - `struct status_t { state_e state; std::string uuid; std::string token; std::string name; }`
  - `struct match_t { std::string request_id; std::string name; }`
  - `std::optional<std::string> request(const std::string &address, std::string name, std::string nonce, const crypto::sha256_t &proof, clock_t::time_point now)`. Returns the request id, or nothing when four requests are pending.
  - `std::optional<match_t> submit_pin(std::string_view pin, const crypto::sha256_t &fingerprint, clock_t::time_point now)`
  - `void complete(const std::string &request_id, std::string name, std::string uuid, std::string token, clock_t::time_point now)`
  - `status_t status(const std::string &request_id, clock_t::time_point now)`. A `paired` result is returned once, then the request is deleted.
  - `std::size_t pending_count() const`

Every method takes the current time as a parameter, so the tests control expiry without sleeping.

- [ ] **Step 1: Write the failing tests**

Create `tests/unit/test_mic_pairing.cpp`:

```cpp
/**
 * @file tests/unit/test_mic_pairing.cpp
 * @brief Test src/mic_pairing.*.
 */
#include <gtest/gtest.h>

#include <src/mic_pairing.h>
#include <src/mic_protocol.h>

namespace {
  using namespace std::chrono_literals;
  using mic::pairing_t;
  using state_e = mic::pairing_t::state_e;

  const crypto::sha256_t FINGERPRINT = crypto::hash("host-certificate");
  const std::string NONCE(16, 'n');

  crypto::sha256_t proof_for(std::string_view pin) {
    return mic::protocol::pin_proof(pin, FINGERPRINT, NONCE);
  }

  pairing_t::clock_t::time_point t0() {
    return pairing_t::clock_t::time_point {} + 1h;
  }
}  // namespace

TEST(MicPairing, CorrectPinMatchesAndCompletes) {
  pairing_t pairing;
  auto id = pairing.request("192.168.1.20", "iPhone", NONCE, proof_for("4821"), t0());
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(pairing.status(*id, t0()).state, state_e::pending);

  auto match = pairing.submit_pin("4821", FINGERPRINT, t0() + 5s);
  ASSERT_TRUE(match.has_value());
  EXPECT_EQ(match->request_id, *id);
  EXPECT_EQ(match->name, "iPhone");

  pairing.complete(*id, "Living room iPhone", "uuid-1", "token-1", t0() + 5s);
  auto status = pairing.status(*id, t0() + 6s);
  EXPECT_EQ(status.state, state_e::paired);
  EXPECT_EQ(status.uuid, "uuid-1");
  EXPECT_EQ(status.token, "token-1");
  EXPECT_EQ(status.name, "Living room iPhone");
}

TEST(MicPairing, TokenIsReturnedOnce) {
  pairing_t pairing;
  auto id = pairing.request("192.168.1.20", "iPhone", NONCE, proof_for("4821"), t0());
  pairing.submit_pin("4821", FINGERPRINT, t0());
  pairing.complete(*id, "iPhone", "uuid-1", "token-1", t0());
  EXPECT_EQ(pairing.status(*id, t0()).state, state_e::paired);
  EXPECT_EQ(pairing.status(*id, t0()).state, state_e::expired);
}

TEST(MicPairing, WrongFingerprintFails) {
  pairing_t pairing;
  pairing.request("192.168.1.20", "iPhone", NONCE, proof_for("4821"), t0());
  EXPECT_FALSE(pairing.submit_pin("4821", crypto::hash("other-certificate"), t0()).has_value());
}

TEST(MicPairing, ThreeWrongPinsCancelEveryPendingRequest) {
  pairing_t pairing;
  auto id = pairing.request("192.168.1.20", "iPhone", NONCE, proof_for("4821"), t0());
  EXPECT_FALSE(pairing.submit_pin("0000", FINGERPRINT, t0()).has_value());
  EXPECT_FALSE(pairing.submit_pin("0001", FINGERPRINT, t0()).has_value());
  EXPECT_EQ(pairing.pending_count(), 1u);
  EXPECT_FALSE(pairing.submit_pin("0002", FINGERPRINT, t0()).has_value());
  EXPECT_EQ(pairing.pending_count(), 0u);
  EXPECT_EQ(pairing.status(*id, t0()).state, state_e::expired);
  EXPECT_FALSE(pairing.submit_pin("4821", FINGERPRINT, t0()).has_value());
}

TEST(MicPairing, RequestExpiresAfter120Seconds) {
  pairing_t pairing;
  auto id = pairing.request("192.168.1.20", "iPhone", NONCE, proof_for("4821"), t0());
  EXPECT_EQ(pairing.status(*id, t0() + 120s).state, state_e::pending);
  EXPECT_EQ(pairing.status(*id, t0() + 121s).state, state_e::expired);
  EXPECT_FALSE(pairing.submit_pin("4821", FINGERPRINT, t0() + 121s).has_value());
}

TEST(MicPairing, CompletionRestartsTheExpiryPeriod) {
  pairing_t pairing;
  auto id = pairing.request("192.168.1.20", "iPhone", NONCE, proof_for("4821"), t0());
  pairing.submit_pin("4821", FINGERPRINT, t0() + 119s);
  pairing.complete(*id, "iPhone", "uuid-1", "token-1", t0() + 119s);
  EXPECT_EQ(pairing.status(*id, t0() + 125s).state, state_e::paired);
}

TEST(MicPairing, NewRequestFromSameAddressReplacesTheOldOne) {
  pairing_t pairing;
  auto first = pairing.request("192.168.1.20", "iPhone", NONCE, proof_for("1111"), t0());
  auto second = pairing.request("192.168.1.20", "iPhone", NONCE, proof_for("2222"), t0() + 1s);
  EXPECT_EQ(pairing.pending_count(), 1u);
  EXPECT_EQ(pairing.status(*first, t0() + 1s).state, state_e::expired);
  EXPECT_EQ(pairing.status(*second, t0() + 1s).state, state_e::pending);
}

TEST(MicPairing, FifthAddressIsRejected) {
  pairing_t pairing;
  for (int index = 0; index < 4; ++index) {
    EXPECT_TRUE(pairing.request("192.168.1." + std::to_string(20 + index), "device", NONCE, proof_for("4821"), t0()).has_value());
  }
  EXPECT_FALSE(pairing.request("192.168.1.99", "device", NONCE, proof_for("4821"), t0()).has_value());
}

TEST(MicPairing, UnknownRequestIdIsExpired) {
  pairing_t pairing;
  EXPECT_EQ(pairing.status("missing", t0()).state, state_e::expired);
}
```

- [ ] **Step 2: Create the header**

Create `src/mic_pairing.h`:

```cpp
/**
 * @file src/mic_pairing.h
 * @brief Pending remote microphone pairing requests, their limits, and the PIN check.
 */
#pragma once

// standard includes
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>

// local includes
#include "crypto.h"

namespace mic {
  class pairing_t {
  public:
    using clock_t = std::chrono::steady_clock;

    static constexpr std::chrono::seconds EXPIRY {120};
    static constexpr std::size_t MAX_PENDING = 4;
    static constexpr int MAX_WRONG_PINS = 3;

    enum class state_e {
      pending,
      paired,
      expired  ///< Also returned for an unknown request id
    };

    struct status_t {
      state_e state;
      std::string uuid;
      std::string token;
      std::string name;
    };

    struct match_t {
      std::string request_id;
      std::string name;
    };

    std::optional<std::string> request(const std::string &address, std::string name, std::string nonce, const crypto::sha256_t &proof, clock_t::time_point now);
    std::optional<match_t> submit_pin(std::string_view pin, const crypto::sha256_t &fingerprint, clock_t::time_point now);
    void complete(const std::string &request_id, std::string name, std::string uuid, std::string token, clock_t::time_point now);
    status_t status(const std::string &request_id, clock_t::time_point now);
    std::size_t pending_count() const;

  private:
    struct request_t {
      std::string address;
      std::string name;
      std::string nonce;
      crypto::sha256_t proof;
      clock_t::time_point created;
      bool paired = false;
      std::string uuid;
      std::string token;
    };

    void expire(clock_t::time_point now);

    std::map<std::string, request_t> requests;
    int wrong_pins = 0;
  };
}  // namespace mic
```

- [ ] **Step 3: Run the tests and confirm they fail**

Run:

```bash
cmake -S tests/mic_standalone -B build/mic_standalone && ninja -C build/mic_standalone
```

Expected: the build FAILS at link time with undefined `mic::pairing_t::request`.

- [ ] **Step 4: Write the implementation**

Create `src/mic_pairing.cpp`:

```cpp
/**
 * @file src/mic_pairing.cpp
 * @brief Definitions for remote microphone pairing state.
 */
// lib includes
#include <openssl/crypto.h>

// local includes
#include "mic_pairing.h"
#include "mic_protocol.h"

namespace mic {
  void pairing_t::expire(clock_t::time_point now) {
    std::erase_if(requests, [&](const auto &entry) {
      return now - entry.second.created > EXPIRY;
    });
  }

  std::size_t pairing_t::pending_count() const {
    std::size_t count = 0;
    for (const auto &[id, request] : requests) {
      if (!request.paired) {
        ++count;
      }
    }
    return count;
  }

  std::optional<std::string> pairing_t::request(const std::string &address, std::string name, std::string nonce, const crypto::sha256_t &proof, clock_t::time_point now) {
    expire(now);

    // "Get a new PIN" sends a second request from the same address. The new one replaces the old one.
    std::erase_if(requests, [&](const auto &entry) {
      return !entry.second.paired && entry.second.address == address;
    });

    if (pending_count() >= MAX_PENDING) {
      return std::nullopt;
    }

    auto request_id = util::hex_vec(crypto::rand(16));
    requests.emplace(request_id, request_t {address, std::move(name), std::move(nonce), proof, now});
    return request_id;
  }

  std::optional<pairing_t::match_t> pairing_t::submit_pin(std::string_view pin, const crypto::sha256_t &fingerprint, clock_t::time_point now) {
    expire(now);

    for (const auto &[id, request] : requests) {
      if (request.paired) {
        continue;
      }
      auto expected = protocol::pin_proof(pin, fingerprint, request.nonce);
      if (CRYPTO_memcmp(expected.data(), request.proof.data(), expected.size()) == 0) {
        wrong_pins = 0;
        return match_t {id, request.name};
      }
    }

    if (++wrong_pins >= MAX_WRONG_PINS) {
      wrong_pins = 0;
      std::erase_if(requests, [](const auto &entry) {
        return !entry.second.paired;
      });
    }
    return std::nullopt;
  }

  void pairing_t::complete(const std::string &request_id, std::string name, std::string uuid, std::string token, clock_t::time_point now) {
    auto it = requests.find(request_id);
    if (it == requests.end()) {
      return;
    }
    it->second.paired = true;
    it->second.name = std::move(name);
    it->second.uuid = std::move(uuid);
    it->second.token = std::move(token);
    // Calliope polls every 2 seconds. A fresh period stops a late PIN entry from losing the token.
    it->second.created = now;
  }

  pairing_t::status_t pairing_t::status(const std::string &request_id, clock_t::time_point now) {
    expire(now);

    auto it = requests.find(request_id);
    if (it == requests.end()) {
      return {state_e::expired, {}, {}, {}};
    }
    if (!it->second.paired) {
      return {state_e::pending, {}, {}, {}};
    }

    status_t result {state_e::paired, it->second.uuid, it->second.token, it->second.name};
    requests.erase(it);
    return result;
  }
}  // namespace mic
```

- [ ] **Step 5: Run the whole standalone suite and confirm it passes**

Run:

```bash
ninja -C build/mic_standalone && ./build/mic_standalone/test_mic_standalone
```

Expected: 36 tests from 4 suites, all PASS.

- [ ] **Step 6: Add the sources to the Apollo build and commit**

In `cmake/compile_definitions/common.cmake`, after the `mic_store.h` line, add:

```cmake
        "${CMAKE_SOURCE_DIR}/src/mic_pairing.cpp"
        "${CMAKE_SOURCE_DIR}/src/mic_pairing.h"
```

```bash
git add src/mic_pairing.h src/mic_pairing.cpp tests/unit/test_mic_pairing.cpp cmake/compile_definitions/common.cmake
git commit -m "feat(mic): add pairing state with PIN proof and limits"
```

---

### Task 10: Jitter buffer retune from the playout research

Run this task after Task 4 and before Task 5.

**Files:**
- Modify: `src/mic_jitter.h`
- Modify: `src/mic_jitter.cpp`
- Modify: `tests/unit/test_mic_jitter.cpp`

**Interfaces:**
- Consumes: `mic::jitter_buffer_t` from Task 2.
- Produces: the same interface with two behaviour changes.
  - `MAX_FRAMES` is 5 (100 ms).
  - New constant `static constexpr int MAX_EMPTY_CONCEAL = 5`. While playing, an empty buffer returns `lost` for up to 5 consecutive calls, then returns `wait` and restarts the prebuffer.

Evidence: `docs/superpowers/research/2026-09-20-voice-playout.md`. With one playout clock, 200 ms of queue is delay with no benefit, and the report's delay guidance puts the cap at 1.5 to 2 times the 40 ms prebuffer. Neither reference implementation restarts its prebuffer on a momentary underrun, and Opus loss concealment is silent by the fifth frame, so 5 concealed frames bound the cost of one late packet.

- [ ] **Step 1: Change the tests first**

In `tests/unit/test_mic_jitter.cpp`, replace the test `DropsOldestAboveMaximum` with:

```cpp
TEST(MicJitter, DropsOldestAboveMaximum) {
  jitter_buffer_t buffer;
  for (std::uint32_t sequence = 0; sequence < 7; ++sequence) {
    buffer.push(sequence, frame(static_cast<std::uint8_t>(sequence)));
  }
  EXPECT_EQ(buffer.size(), jitter_buffer_t::MAX_FRAMES);
  auto first = buffer.pop();
  ASSERT_EQ(first.kind, pop_e::frame);
  EXPECT_EQ(first.payload, frame(2));
}
```

Replace the test `EmptyBufferRestartsPrebuffer` with these two tests:

```cpp
TEST(MicJitter, EmptyBufferConcealsThenRestartsPrebuffer) {
  jitter_buffer_t buffer;
  buffer.push(0, frame(0));
  buffer.push(1, frame(1));
  buffer.pop();
  buffer.pop();
  for (int call = 0; call < jitter_buffer_t::MAX_EMPTY_CONCEAL; ++call) {
    EXPECT_EQ(buffer.pop().kind, pop_e::lost) << "call " << call;
  }
  EXPECT_EQ(buffer.pop().kind, pop_e::wait);
  buffer.push(40, frame(4));
  EXPECT_EQ(buffer.pop().kind, pop_e::wait);
  buffer.push(41, frame(5));
  auto next = buffer.pop();
  ASSERT_EQ(next.kind, pop_e::frame);
  EXPECT_EQ(next.payload, frame(4));
}

TEST(MicJitter, ResumesWithoutPrebufferAfterAShortGap) {
  jitter_buffer_t buffer;
  buffer.push(0, frame(0));
  buffer.push(1, frame(1));
  buffer.pop();
  buffer.pop();
  EXPECT_EQ(buffer.pop().kind, pop_e::lost);  // sequence 2 concealed
  EXPECT_FALSE(buffer.push(2, frame(2)));  // arrives after its concealment: late
  EXPECT_TRUE(buffer.push(3, frame(3)));
  auto next = buffer.pop();
  ASSERT_EQ(next.kind, pop_e::frame);
  EXPECT_EQ(next.payload, frame(3));
}
```

- [ ] **Step 2: Run the tests and confirm the three fail**

Run:

```bash
ninja -C build/mic_standalone && ./build/mic_standalone/test_mic_standalone --gtest_filter='MicJitter.*'
```

Expected: the build FAILS, because `MAX_EMPTY_CONCEAL` is not declared.

- [ ] **Step 3: Change the header**

In `src/mic_jitter.h`, replace the line `static constexpr std::size_t MAX_FRAMES = 10;  // 200 ms` with:

```cpp
    static constexpr std::size_t MAX_FRAMES = 5;  // 100 ms
    static constexpr int MAX_EMPTY_CONCEAL = 5;  // Opus concealment is silent by the fifth frame
```

In the private section, after `std::uint32_t next_sequence = 0;`, add:

```cpp
    int empty_pops = 0;
```

Change the comment on `pop_e::wait` to `///< Nothing to play: prebuffering, or the talkspurt ended` and the comment on `pop_e::lost` to `///< The next sequence is missing or the buffer ran dry. Run loss concealment.`

- [ ] **Step 4: Change the implementation**

In `src/mic_jitter.cpp`, in `pop()`, replace:

```cpp
    if (frames.empty()) {
      started = false;
      return {pop_e::wait, {}};
    }
```

with:

```cpp
    if (frames.empty()) {
      // A momentary underrun is concealed. Restarting the prebuffer for it would add a 40 ms stall.
      if (++empty_pops > MAX_EMPTY_CONCEAL) {
        started = false;
        empty_pops = 0;
        return {pop_e::wait, {}};
      }
      ++next_sequence;
      return {pop_e::lost, {}};
    }
    empty_pops = 0;
```

- [ ] **Step 5: Run the tests and confirm they pass**

Run:

```bash
ninja -C build/mic_standalone && ./build/mic_standalone/test_mic_standalone
```

Expected: 37 tests from 4 suites, all PASS. `MicJitter` has 10 tests.

- [ ] **Step 6: Commit**

```bash
git add src/mic_jitter.h src/mic_jitter.cpp tests/unit/test_mic_jitter.cpp
git commit -m "feat(mic): cap the jitter buffer at 100 ms and conceal short underruns"
```

---

### Task 5: Platform interface and the Windows virtual microphone writer

**Files:**
- Modify: `src/platform/common.h` (after `class mic_t`, near line 553)
- Create: `src/platform/windows/mic_write.cpp`
- Modify: `src/platform/linux/audio.cpp` (end of `namespace platf`)
- Modify: `src/platform/macos/microphone.mm` (end of `namespace platf`)
- Modify: `cmake/compile_definitions/windows.cmake:61`

**Interfaces:**
- Consumes: `util::safe_ptr`, `platf::from_utf8`, `platf::to_utf8` from `src/platform/windows/misc.h`, `IPolicyConfig` from `src/platform/windows/PolicyConfig.h`, `config::audio.install_steam_drivers`.
- Produces, in namespace `platf`:
  - `constexpr std::size_t VIRTUAL_MIC_MAX_PACKET_SAMPLES = 5760`
  - `using virtual_mic_fill_t = std::function<std::size_t(float *mono_out, std::size_t capacity)>`. The callback decodes one packet into `mono_out` and returns the sample count. It returns 0 when nothing is ready. The render thread is the only caller.
  - `class virtual_mic_t` with `virtual bool healthy() const = 0` and `virtual std::string previous_default_capture() const = 0`
  - `enum class virtual_mic_error_e { none, unsupported, device_missing, device_open_failed }`
  - `std::unique_ptr<virtual_mic_t> virtual_mic(virtual_mic_fill_t fill, virtual_mic_error_e &error_out)`. Destruction joins the render thread and restores the previous default capture device.
  - `void restore_default_capture(const std::string &device_id)`

This task has no unit test. WASAPI and the Steam driver exist only on the Windows PC, so the check is a Windows build followed by the manual verification in Task 9.

Design evidence is in `docs/superpowers/research/2026-09-20-windows-virtual-mic.md` and `docs/superpowers/research/2026-09-20-voice-playout.md`. The points that shape this file:

- **Pull playout.** The WASAPI render event is the only playout clock. The render thread asks the fill callback for one packet at a time. No timer and no sample queue exist between the jitter buffer and the device.
- **Device queue target.** The render thread requests packets only while the device holds less than 40 ms. The jitter buffer therefore holds the delay, and the device buffer does not add to it.
- **Forced endpoint format.** The Steam driver copies audio from its render endpoint to its capture endpoint with no conversion. Both endpoints are set to 2 channels, 32-bit PCM, 48000 Hz through `IPolicyConfig::SetDeviceFormat`. A mismatch between the two produces garbled voice. The device format uses `KSDATAFORMAT_SUBTYPE_PCM`, because the float subtype makes `Initialize` fail with `0x88890008`. The stream format is float.
- **200 ms device buffer, primed with silence.** A 100 ms buffer underruns at a 20 ms packet cadence. Half the buffer is filled with silence before `Start()`.
- **Recovery.** `AUDCLNT_E_DEVICE_INVALIDATED`, `AUDCLNT_E_RESOURCES_INVALIDATED`, and `AUDCLNT_E_SERVICE_NOT_RUNNING` reopen the client, and queued audio from before the reopen is discarded.
- **Thread contract.** The object is created and destroyed on one thread, which is no single-threaded COM apartment. `init()` refuses `RPC_E_CHANGED_MODE`, and the destructor logs a destroy on a foreign thread. The mic module uses the mic thread for both.
- **Self-sufficient GUIDs.** The file defines `INITGUID` before its includes, as `src/platform/windows/audio.cpp` does. MinGW-w64 `DEFINE_GUID` only declares a GUID without it. `Audioclient.h` already includes `ksmedia.h`.
- **Previous default first.** The default capture device is read before the driver install, because the install can make Windows promote the new device.
- **Parameter name.** The factory's out-parameter is `error_out`. A parameter named `error` hides Apollo's Boost.Log `error` logger and breaks `BOOST_LOG(error)`.

The device lookup, the format normalisation, and the render loop are adapted from Apollo pull request #1428, which is GPL-3.0 like Apollo.

- [ ] **Step 1: Add the interface to `src/platform/common.h`**

Confirm that `src/platform/common.h` includes `<functional>`. When it does not, add `#include <functional>` to its standard includes.

After the closing brace of `class mic_t` and before `class audio_control_t`, add:

```cpp
  /// The longest Opus packet is 120 ms, which is 5760 samples at 48 kHz.
  constexpr std::size_t VIRTUAL_MIC_MAX_PACKET_SAMPLES = 5760;

  /**
   * @brief Supplies decoded 48 kHz mono float audio to a virtual microphone, one packet per call.
   * @param mono_out Receives the samples. Holds at least VIRTUAL_MIC_MAX_PACKET_SAMPLES.
   * @return The number of samples written, or 0 when nothing is ready to play.
   *
   * Called only from the virtual microphone's render thread.
   * The callback can run until the virtual microphone's destructor returns, so everything it
   * captures must outlive that destructor.
   */
  using virtual_mic_fill_t = std::function<std::size_t(float *mono_out, std::size_t capacity)>;

  /**
   * @brief A host capture device that plays audio supplied by Apollo.
   *
   * On Windows this is the Steam Streaming Microphone. Creating one makes it the default
   * capture device. Destroying it stops the render thread and restores the previous default.
   * Create and destroy the object on the same thread. That thread must not be a single-threaded
   * COM apartment on Windows.
   */
  class virtual_mic_t {
  public:
    /**
     * @return false after the device failed and could not be reopened.
     */
    virtual bool healthy() const = 0;

    /**
     * @brief The default capture device id that was active before this object took over.
     */
    virtual std::string previous_default_capture() const = 0;

    virtual ~virtual_mic_t() = default;
  };

  enum class virtual_mic_error_e {
    none,
    unsupported,  ///< This platform has no virtual microphone
    device_missing,  ///< The virtual microphone is not installed
    device_open_failed  ///< The virtual microphone exists and cannot be opened
  };

  /**
   * @brief Open the virtual microphone on the calling thread and start pulling audio from fill.
   * @param error_out Receives the reason when the result is null.
   *
   * Can block for several seconds when the driver is installed.
   */
  std::unique_ptr<virtual_mic_t> virtual_mic(virtual_mic_fill_t fill, virtual_mic_error_e &error_out);

  /**
   * @brief Make the given device the default capture device. Used after a crash during a mic session.
   */
  void restore_default_capture(const std::string &device_id);
```

- [ ] **Step 2: Add the factories for Linux and macOS**

At the end of `namespace platf` in `src/platform/linux/audio.cpp`, add:

```cpp
  std::unique_ptr<virtual_mic_t> virtual_mic([[maybe_unused]] virtual_mic_fill_t fill, virtual_mic_error_e &error_out) {
    error_out = virtual_mic_error_e::unsupported;
    return nullptr;
  }

  void restore_default_capture([[maybe_unused]] const std::string &device_id) {
  }
```

Add the same two functions at the end of `namespace platf` in `src/platform/macos/microphone.mm`.

- [ ] **Step 3: Write the Windows writer**

Create `src/platform/windows/mic_write.cpp`:

```cpp
/**
 * @file src/platform/windows/mic_write.cpp
 * @brief Plays audio into the Steam Streaming Microphone and manages the default capture device.
 *
 * Device lookup, endpoint format normalisation, and the event-driven render loop are adapted
 * from Apollo pull request #1428 (logabell/apollo-microphone), licensed GPL-3.0.
 * Design evidence: docs/superpowers/research/2026-09-20-windows-virtual-mic.md and
 * docs/superpowers/research/2026-09-20-voice-playout.md.
 */
#define INITGUID
// standard includes
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <optional>
#include <thread>
#include <vector>

// platform includes
#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <newdev.h>
#include <synchapi.h>

// local includes
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"

// Must be the last included file
// clang-format off
#include "PolicyConfig.h"
// clang-format on

using namespace std::literals;

namespace platf {
  namespace {
    constexpr PROPERTYKEY MIC_PKEY_DEVICE_DESC {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2};
    constexpr PROPERTYKEY MIC_PKEY_FRIENDLY_NAME {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
    constexpr PROPERTYKEY MIC_PKEY_INTERFACE_NAME {{0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}}, 2};

    constexpr DWORD SAMPLE_RATE = 48000;
    // 100 ms underruns at a 20 ms packet cadence (moonlight-mic raised its buffer to 200 ms for this).
    constexpr REFERENCE_TIME BUFFER_DURATION_100NS = 2000000;
    // Request packets only while the device holds less than 40 ms, so the jitter buffer holds the delay.
    constexpr UINT32 TARGET_QUEUED_FRAMES = SAMPLE_RATE / 25;
    // The device description and the interface name come from the driver INF and are not localised.
    constexpr auto DEVICE_NAME_PATTERN = L"steam streaming microphone";

#if defined(__x86_64) || defined(__x86_64__) || defined(__amd64) || defined(__amd64__) || defined(_M_AMD64)
    constexpr auto STEAM_MIC_DRIVER_PATH = L"%CommonProgramFiles(x86)%\\Steam\\drivers\\Windows10\\x64\\SteamStreamingMicrophone.inf";
#else
    constexpr auto STEAM_MIC_DRIVER_PATH = L"";
#endif

    template<class T>
    void release_com(T *pointer) {
      if (pointer) {
        pointer->Release();
      }
    }

    template<class T>
    void co_task_free(T *pointer) {
      if (pointer) {
        CoTaskMemFree(pointer);
      }
    }

    using device_enum_t = util::safe_ptr<IMMDeviceEnumerator, release_com<IMMDeviceEnumerator>>;
    using device_t = util::safe_ptr<IMMDevice, release_com<IMMDevice>>;
    using collection_t = util::safe_ptr<IMMDeviceCollection, release_com<IMMDeviceCollection>>;
    using prop_t = util::safe_ptr<IPropertyStore, release_com<IPropertyStore>>;
    using policy_t = util::safe_ptr<IPolicyConfig, release_com<IPolicyConfig>>;
    using audio_client_t = util::safe_ptr<IAudioClient, release_com<IAudioClient>>;
    using render_client_t = util::safe_ptr<IAudioRenderClient, release_com<IAudioRenderClient>>;
    using wstring_t = util::safe_ptr<WCHAR, co_task_free<WCHAR>>;
    using wave_format_t = util::safe_ptr<WAVEFORMATEX, co_task_free<WAVEFORMATEX>>;

    std::wstring lower(std::wstring text) {
      std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
      });
      return text;
    }

    std::wstring prop_string(IPropertyStore *store, const PROPERTYKEY &key) {
      PROPVARIANT value;
      PropVariantInit(&value);
      std::wstring text;
      if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
        text = value.pwszVal;
      }
      PropVariantClear(&value);
      return text;
    }

    device_enum_t create_enumerator() {
      device_enum_t device_enum;
      CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void **) &device_enum);
      return device_enum;
    }

    policy_t create_policy() {
      policy_t policy;
      CoCreateInstance(CLSID_CPolicyConfigClient, nullptr, CLSCTX_ALL, IID_IPolicyConfig, (void **) &policy);
      return policy;
    }

    /**
     * @brief Find the active Steam Streaming Microphone endpoint for one data flow.
     *
     * Render and capture are looked up separately. A match on the name alone once made
     * another implementation write to the wrong endpoint.
     */
    std::optional<std::wstring> find_steam_endpoint(IMMDeviceEnumerator *device_enum, EDataFlow flow) {
      collection_t collection;
      if (FAILED(device_enum->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection)) || !collection) {
        return std::nullopt;
      }

      UINT count = 0;
      collection->GetCount(&count);
      for (UINT index = 0; index < count; ++index) {
        device_t device;
        wstring_t id;
        prop_t props;
        if (FAILED(collection->Item(index, &device)) || !device ||
            FAILED(device->GetId(&id)) || !id ||
            FAILED(device->OpenPropertyStore(STGM_READ, &props)) || !props) {
          continue;
        }

        for (const auto &key : {MIC_PKEY_FRIENDLY_NAME, MIC_PKEY_INTERFACE_NAME, MIC_PKEY_DEVICE_DESC}) {
          if (lower(prop_string(props.get(), key)).find(DEVICE_NAME_PATTERN) != std::wstring::npos) {
            return std::wstring {id.get()};
          }
        }
      }
      return std::nullopt;
    }

    /**
     * @brief Install the Steam Streaming Microphone driver from the Steam folder.
     */
    bool install_steam_mic_driver() {
      if (!config::audio.install_steam_drivers) {
        return false;
      }
      if (!*STEAM_MIC_DRIVER_PATH) {
        return false;
      }

      // MinGW's libnewdev.a is missing DiInstallDriverW(), so it is loaded at runtime.
      auto newdev = LoadLibraryExW(L"newdev.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!newdev) {
        return false;
      }
      auto fg = util::fail_guard([newdev]() {
        FreeLibrary(newdev);
      });

      auto fn_DiInstallDriverW = (decltype(DiInstallDriverW) *) GetProcAddress(newdev, "DiInstallDriverW");
      if (!fn_DiInstallDriverW) {
        return false;
      }

      WCHAR driver_path[MAX_PATH] = {};
      ExpandEnvironmentStringsW(STEAM_MIC_DRIVER_PATH, driver_path, ARRAYSIZE(driver_path));
      if (!fn_DiInstallDriverW(nullptr, driver_path, 0, nullptr)) {
        auto code = GetLastError();
        switch (code) {
          case ERROR_ACCESS_DENIED:
            BOOST_LOG(warning) << "Remote microphone: administrator privileges are required to install the Steam Streaming Microphone"sv;
            break;
          case ERROR_FILE_NOT_FOUND:
          case ERROR_PATH_NOT_FOUND:
            BOOST_LOG(info) << "Remote microphone: the Steam audio drivers were not found. Steam is not installed on this PC."sv;
            break;
          default:
            BOOST_LOG(warning) << "Remote microphone: could not install the Steam Streaming Microphone driver: "sv << code;
            break;
        }
        return false;
      }

      BOOST_LOG(info) << "Remote microphone: installed the Steam Streaming Microphone driver"sv;
      // The audio subsystem needs time to publish the new endpoints.
      Sleep(5000);
      return true;
    }

    WAVEFORMATEXTENSIBLE make_format(bool ieee_float) {
      WAVEFORMATEXTENSIBLE format {};
      format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
      format.Format.nChannels = 2;
      format.Format.nSamplesPerSec = SAMPLE_RATE;
      format.Format.wBitsPerSample = 32;
      format.Format.nBlockAlign = static_cast<WORD>(format.Format.nChannels * (format.Format.wBitsPerSample / 8));
      format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
      format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
      format.Samples.wValidBitsPerSample = 32;
      format.SubFormat = ieee_float ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
      format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
      return format;
    }

    bool is_normalised(const WAVEFORMATEX *format) {
      return format && format->nChannels == 2 && format->nSamplesPerSec == SAMPLE_RATE && format->wBitsPerSample == 32;
    }

    /**
     * @brief Set one endpoint to 2 channels, 32-bit PCM, 48000 Hz.
     *
     * The Steam driver copies render to capture with no conversion, so both endpoints need the
     * same format. The device format must be PCM: the float subtype makes Initialize fail with
     * 0x88890008.
     */
    void normalise_endpoint_format(IPolicyConfig *policy, const std::wstring &device_id) {
      wave_format_t current;
      if (SUCCEEDED(policy->GetDeviceFormat(device_id.c_str(), FALSE, &current)) && is_normalised(current.get())) {
        return;
      }

      auto wanted = make_format(false);
      WAVEFORMATEXTENSIBLE previous {};
      auto status = policy->SetDeviceFormat(device_id.c_str(), &wanted.Format, &previous.Format);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Remote microphone: could not set the Steam Streaming Microphone format: 0x"sv << util::hex(status).to_string_view();
      }
    }

    std::wstring default_capture_id(IMMDeviceEnumerator *device_enum) {
      device_t device;
      wstring_t id;
      if (FAILED(device_enum->GetDefaultAudioEndpoint(eCapture, eConsole, &device)) || !device ||
          FAILED(device->GetId(&id)) || !id) {
        return {};
      }
      return id.get();
    }

    void set_default_capture(const std::wstring &device_id) {
      auto policy = create_policy();
      if (!policy || device_id.empty()) {
        return;
      }
      for (int role = 0; role < (int) ERole_enum_count; ++role) {
        policy->SetDefaultEndpoint(device_id.c_str(), (ERole) role);
      }
    }

    bool is_recoverable(HRESULT status) {
      return status == AUDCLNT_E_DEVICE_INVALIDATED ||
             status == AUDCLNT_E_RESOURCES_INVALIDATED ||
             status == AUDCLNT_E_SERVICE_NOT_RUNNING;
    }

    class wasapi_virtual_mic_t: public virtual_mic_t {
    public:
      explicit wasapi_virtual_mic_t(virtual_mic_fill_t fill):
          fill {std::move(fill)} {
      }

      ~wasapi_virtual_mic_t() override {
        if (owner != std::thread::id {} && std::this_thread::get_id() != owner) {
          BOOST_LOG(error) << "Remote microphone: the virtual microphone was destroyed on a different thread from the one that created it. The default capture device may not be restored."sv;
        }
        stop = true;
        if (render_thread.joinable()) {
          render_thread.join();
        }
        close_client();
        if (!previous_default.empty()) {
          set_default_capture(previous_default);
        }
        if (render_event) {
          CloseHandle(render_event);
        }
        device_enum.reset();
        if (com_initialized) {
          CoUninitialize();
        }
      }

      bool init(virtual_mic_error_e &error_out) {
        owner = std::this_thread::get_id();

        auto com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY);
        if (com_status == RPC_E_CHANGED_MODE) {
          BOOST_LOG(error) << "Remote microphone: the virtual microphone must be created on a thread that is not a single-threaded COM apartment"sv;
          error_out = virtual_mic_error_e::device_open_failed;
          return false;
        }
        com_initialized = SUCCEEDED(com_status);

        device_enum = create_enumerator();
        if (!device_enum) {
          error_out = virtual_mic_error_e::device_open_failed;
          return false;
        }

        previous_default = default_capture_id(device_enum.get());

        auto found_render = find_steam_endpoint(device_enum.get(), eRender);
        if (!found_render && install_steam_mic_driver()) {
          found_render = find_steam_endpoint(device_enum.get(), eRender);
        }
        auto found_capture = find_steam_endpoint(device_enum.get(), eCapture);
        if (!found_render || !found_capture) {
          BOOST_LOG(warning) << "Remote microphone: the Steam Streaming Microphone was not found. Install Steam on this PC."sv;
          error_out = virtual_mic_error_e::device_missing;
          return false;
        }
        render_id = *found_render;

        if (auto policy = create_policy()) {
          normalise_endpoint_format(policy.get(), render_id);
          normalise_endpoint_format(policy.get(), *found_capture);
        }

        render_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!render_event || !open_client()) {
          error_out = virtual_mic_error_e::device_open_failed;
          return false;
        }

        if (previous_default == *found_capture) {
          // A stale switch is already in place. Keeping it as "previous" would make the restore a no-op forever.
          previous_default.clear();
        }
        set_default_capture(*found_capture);

        render_thread = std::thread {[this]() {
          render_loop();
        }};
        return true;
      }

      bool healthy() const override {
        return !failed;
      }

      std::string previous_default_capture() const override {
        return to_utf8(previous_default);
      }

    private:
      /**
       * @brief Activate, initialise, prime, and start the render client. Runs on init and on recovery.
       */
      bool open_client() {
        device_t device;
        if (FAILED(device_enum->GetDevice(render_id.c_str(), &device)) || !device ||
            FAILED(device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void **) &audio_client)) || !audio_client) {
          return false;
        }

        auto format = make_format(true);
        auto status = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, BUFFER_DURATION_100NS, 0, &format.Format, nullptr);
        if (FAILED(status)) {
          BOOST_LOG(error) << "Remote microphone: could not initialize the Steam Streaming Microphone: 0x"sv << util::hex(status).to_string_view();
          return false;
        }

        if (FAILED(audio_client->GetBufferSize(&buffer_frames)) ||
            FAILED(audio_client->GetService(IID_IAudioRenderClient, (void **) &render_client)) || !render_client ||
            FAILED(audio_client->SetEventHandle(render_event))) {
          return false;
        }

        // A cold buffer glitches at the start of the stream, so half of it is primed with silence.
        BYTE *buffer = nullptr;
        if (SUCCEEDED(render_client->GetBuffer(buffer_frames / 2, &buffer)) && buffer) {
          render_client->ReleaseBuffer(buffer_frames / 2, AUDCLNT_BUFFERFLAGS_SILENT);
        }

        return SUCCEEDED(audio_client->Start());
      }

      void close_client() {
        if (audio_client) {
          audio_client->Stop();
        }
        render_client.reset();
        audio_client.reset();
      }

      /**
       * @brief Reopen the client after a recoverable error. Latches failed for every other error.
       */
      bool recover(HRESULT status) {
        if (!is_recoverable(status)) {
          failed = true;
          return false;
        }

        BOOST_LOG(warning) << "Remote microphone: reopening the Steam Streaming Microphone after 0x"sv << util::hex(status).to_string_view();
        close_client();
        Sleep(500);
        if (!open_client()) {
          failed = true;
          return false;
        }
        return true;
      }

      void render_loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY);
        platf::adjust_thread_priority(platf::thread_priority_e::high);

        std::vector<float> packet(VIRTUAL_MIC_MAX_PACKET_SAMPLES);
        std::vector<float> pending;  // decoded samples that did not fit in the device yet

        while (!stop) {
          WaitForSingleObject(render_event, 20);
          if (stop) {
            break;
          }

          UINT32 padding = 0;
          auto status = audio_client->GetCurrentPadding(&padding);
          if (FAILED(status)) {
            if (!recover(status)) {
              break;
            }
            pending.clear();  // stale audio must not replay into the reopened client
            continue;
          }

          while (padding + pending.size() < TARGET_QUEUED_FRAMES) {
            auto samples = fill(packet.data(), packet.size());
            samples = std::min(samples, packet.size());
            if (samples == 0) {
              break;
            }
            for (std::size_t index = 0; index < samples; ++index) {
              pending.push_back(std::clamp(packet[index], -1.0f, 1.0f));
            }
          }

          auto frames = std::min<UINT32>(buffer_frames - padding, static_cast<UINT32>(pending.size()));
          if (frames == 0) {
            continue;
          }

          BYTE *buffer = nullptr;
          status = render_client->GetBuffer(frames, &buffer);
          if (FAILED(status)) {
            if (!recover(status)) {
              break;
            }
            pending.clear();
            continue;
          }
          if (!buffer) {
            continue;
          }

          auto *out = reinterpret_cast<float *>(buffer);
          for (UINT32 frame = 0; frame < frames; ++frame) {
            out[frame * 2] = pending[frame];
            out[frame * 2 + 1] = pending[frame];
          }
          pending.erase(pending.begin(), pending.begin() + frames);

          status = render_client->ReleaseBuffer(frames, 0);
          if (FAILED(status) && !recover(status)) {
            break;
          }
        }

        CoUninitialize();
      }

      virtual_mic_fill_t fill;
      device_enum_t device_enum;
      audio_client_t audio_client;
      render_client_t render_client;
      HANDLE render_event = nullptr;
      UINT32 buffer_frames = 0;
      std::wstring render_id;
      std::wstring previous_default;
      bool com_initialized = false;

      std::thread render_thread;
      std::atomic<bool> stop {false};
      std::atomic<bool> failed {false};
      std::thread::id owner;
    };
  }  // namespace

  std::unique_ptr<virtual_mic_t> virtual_mic(virtual_mic_fill_t fill, virtual_mic_error_e &error_out) {
    error_out = virtual_mic_error_e::none;
    auto mic = std::make_unique<wasapi_virtual_mic_t>(std::move(fill));
    if (!mic->init(error_out)) {
      return nullptr;
    }
    return mic;
  }

  void restore_default_capture(const std::string &device_id) {
    bool com_initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY));
    set_default_capture(from_utf8(device_id));
    if (com_initialized) {
      CoUninitialize();
    }
  }
}  // namespace platf
```

- [ ] **Step 4: Add the source to the Windows build**

In `cmake/compile_definitions/windows.cmake`, after the line `"${CMAKE_SOURCE_DIR}/src/platform/windows/audio.cpp"`, add:

```cmake
        "${CMAKE_SOURCE_DIR}/src/platform/windows/mic_write.cpp"
```

- [ ] **Step 5: Local check of the shared header**

Run:

```bash
python3 tests/mic_standalone/syntax_check.py src/confighttp.cpp
```

Expected: exit code 0. `src/confighttp.cpp` includes `src/platform/common.h`, so this confirms that the new interface parses.

- [ ] **Step 6: Windows check**

On the Windows PC, the operator pulls the branch and runs:

```bash
cmake -B cmake-build-mic -G Ninja -S . -DBUILD_TESTS=ON && ninja -C cmake-build-mic
```

Expected: the build completes. `mic_write.cpp` compiles and links with no error.

- [ ] **Step 7: Commit**

```bash
git add src/platform/common.h src/platform/windows/mic_write.cpp src/platform/linux/audio.cpp src/platform/macos/microphone.mm cmake/compile_definitions/windows.cmake
git commit -m "feat(mic): add virtual microphone interface and Windows WASAPI writer"
```

---

### Task 6: Mic module orchestrator, tray functions, and startup

**Files:**
- Create: `src/mic.h`
- Create: `src/mic.cpp`
- Modify: `src/system_tray.h` (after `update_tray_client_connected`, near line 84)
- Modify: `src/system_tray.cpp` (after the body of `update_tray_client_connected`)
- Modify: `src/main.cpp:12-24` (includes), `:430` (start), `:459` (stop)
- Modify: `cmake/compile_definitions/common.cmake` (after the `mic_pairing` lines)

**Interfaces:**
- Consumes: everything produced by Tasks 1 to 5.
- Produces, in namespace `mic`:
  - `enum class session_error_e { unsupported, failed }`
  - `struct session_info_t { std::uint32_t session_id; std::string key; std::uint16_t port; }`. `key` holds 16 raw bytes.
  - `void start()`, `void stop()`
  - `std::optional<std::string> pair_request(const std::string &address, const std::string &name, const std::string &nonce, const crypto::sha256_t &proof)`
  - `pairing_t::status_t pair_status(const std::string &request_id)`
  - `std::optional<std::string> submit_pin(const std::string &pin, const std::string &name)`. Returns the name of the paired mic device.
  - `nlohmann::json list()`. An array of `{name, uuid, connected}`.
  - `bool remove(const std::string &uuid)`
  - `std::optional<device_t> authorize(const std::string &token)`
  - `std::variant<session_info_t, session_error_e> session_start(const device_t &device)`
  - `void session_end(const std::string &uuid)`
- Produces, in namespace `system_tray`:
  - `void update_tray_mic_pair_request()`
  - `void update_tray_mic_connected(std::string device_name)`
  - `void update_tray_mic_disconnected(std::string device_name, std::string reason)`
  - `void update_tray_mic_error(std::string message)`

Threading model, three threads:

- **Mic thread.** Runs one `io_context`. It owns the socket, the mic session, and a 1 second housekeeping timer. It pushes audio frames into the jitter buffer.
- **Render thread.** Owned by the virtual microphone from Task 5. It calls `fill_from_session()`, which pops one frame from the jitter buffer and decodes it. The decoder is touched only here. The audio device clock is therefore the only playout clock.
- **Config server thread.** Handlers reach the mic session by posting a task to the mic thread and waiting on a future.

Two mutexes exist. `session_t::jitter_mutex` guards the jitter buffer between the mic thread and the render thread. `state_mutex` guards the store, the pairing state, and the connected device id, which handlers read directly.

`end_session()` destroys the virtual microphone before the decoder. That order joins the render thread first, so the decoder has no user when it is destroyed.

Design evidence: `docs/superpowers/research/2026-09-20-voice-playout.md`. A 20 ms timer is unsuitable on Windows, where the default timer granularity is 15.625 ms and Apollo raises it only while a stream runs.

- [ ] **Step 1: Add the tray functions**

In `src/system_tray.h`, after the declaration of `update_tray_client_connected`, add:

```cpp

  /**
   * @brief Spawns a notification for a remote microphone pairing request. Clicking it opens the Microphone tab.
   */
  void update_tray_mic_pair_request();

  void update_tray_mic_connected(std::string device_name);

  /**
   * @param reason Empty, or a short reason such as "connection lost".
   */
  void update_tray_mic_disconnected(std::string device_name, std::string reason);

  void update_tray_mic_error(std::string message);
```

In `src/system_tray.cpp`, after the closing brace of `update_tray_client_connected`, add:

```cpp
  static void mic_notification(const char *title, const std::string &text, void (*callback)()) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = NULL;
    tray.notification_text = NULL;
    tray.notification_cb = NULL;
    tray.notification_icon = NULL;
    tray.icon = TRAY_ICON;
    tray_update(&tray);
    char msg[256];
    snprintf(msg, std::size(msg), "%s", text.c_str());
  #ifdef _WIN32
    strncpy(msg, utf8ToAcp(msg).c_str(), std::size(msg) - 1);
  #endif
    tray.notification_title = title;
    tray.notification_text = msg;
    tray.notification_icon = TRAY_ICON;
    tray.notification_cb = callback;
    tray.tooltip = PROJECT_NAME;
    tray_update(&tray);
  }

  void update_tray_mic_pair_request() {
    mic_notification("Incoming microphone pairing request", "Click here to enter the PIN from Calliope", []() {
      launch_ui("/pin#MIC");
    });
  }

  void update_tray_mic_connected(std::string device_name) {
    mic_notification("Microphone connected", "Microphone connected: " + device_name, nullptr);
  }

  void update_tray_mic_disconnected(std::string device_name, std::string reason) {
    auto text = "Microphone disconnected: " + device_name;
    if (!reason.empty()) {
      text += " (" + reason + ")";
    }
    mic_notification("Microphone disconnected", text, nullptr);
  }

  void update_tray_mic_error(std::string message) {
    mic_notification("Microphone error", message, nullptr);
  }
```

- [ ] **Step 2: Create the mic module header**

Create `src/mic.h`:

```cpp
/**
 * @file src/mic.h
 * @brief Remote microphone: pairing, mic sessions, and playout into the virtual microphone.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "crypto.h"
#include "mic_pairing.h"
#include "mic_store.h"

namespace mic {
  enum class session_error_e {
    unsupported,  ///< This host platform has no virtual microphone
    failed
  };

  struct session_info_t {
    std::uint32_t session_id;
    std::string key;  ///< 16 raw bytes
    std::uint16_t port;
  };

  /**
   * @brief Load the store and repair a default capture device left switched by a crash.
   *
   * Starts the mic thread only when a mic device is already paired.
   */
  void start();

  /**
   * @brief End the mic session, restore the default capture device, and join the mic thread.
   */
  void stop();

  std::optional<std::string> pair_request(const std::string &address, const std::string &name, const std::string &nonce, const crypto::sha256_t &proof);
  pairing_t::status_t pair_status(const std::string &request_id);
  std::optional<std::string> submit_pin(const std::string &pin, const std::string &name);

  nlohmann::json list();
  bool remove(const std::string &uuid);
  std::optional<device_t> authorize(const std::string &token);

  std::variant<session_info_t, session_error_e> session_start(const device_t &device);
  void session_end(const std::string &uuid);
}  // namespace mic
```

- [ ] **Step 3: Write the mic module**

Create `src/mic.cpp`:

```cpp
/**
 * @file src/mic.cpp
 * @brief Definitions for the remote microphone module.
 *
 * This module never touches streaming code. Every failure ends the mic session only.
 *
 * Playout is pull-based: the virtual microphone's render thread calls fill_from_session()
 * when the device has room, so the audio device clock is the only playout clock.
 * Design evidence: docs/superpowers/research/2026-09-20-voice-playout.md.
 */
// standard includes
#include <array>
#include <atomic>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>

// lib includes
#include <boost/asio.hpp>
#include <opus/opus.h>

// local includes
#include "config.h"
#include "file_handler.h"
#include "logging.h"
#include "mic.h"
#include "mic_jitter.h"
#include "mic_protocol.h"
#include "network.h"
#include "platform/common.h"
#include "system_tray.h"
#include "uuid.h"

using namespace std::literals;

namespace mic {
  namespace {
    namespace asio = boost::asio;
    using asio::ip::udp;
    using steady = std::chrono::steady_clock;

    constexpr int MIC_PORT_OFFSET = 13;
    constexpr int FRAME_SAMPLES = 960;  // 20 ms at 48 kHz, the duration concealed for one lost frame
    constexpr auto HOUSEKEEPING = 1s;
    constexpr auto SESSION_TIMEOUT = 5s;
    constexpr auto REPLACED_LIFETIME = 5s;
    constexpr int MAX_DECODE_FAILURES = 25;  // 500 ms of consecutive failures

    struct session_t {
      std::uint32_t id;
      crypto::cipher::gcm_t cipher;
      device_t device;
      steady::time_point last_valid;
      std::atomic<protocol::error_e> error {protocol::error_e::none};

      // Shared by the mic thread (push) and the render thread (pop).
      std::mutex jitter_mutex;
      jitter_buffer_t jitter;

      // Touched only by the render thread, through fill_from_session().
      OpusDecoder *decoder = nullptr;
      bool waiting = true;
      int decode_failures = 0;

      std::unique_ptr<platf::virtual_mic_t> vmic;
    };

    struct replaced_t {
      std::uint32_t id;
      crypto::cipher::gcm_t cipher;
      steady::time_point expires;
    };

    // Guarded by state_mutex. Read by config server handlers.
    std::mutex state_mutex;
    std::unique_ptr<store_t> store;
    pairing_t pairing;
    crypto::sha256_t fingerprint {};
    std::string connected_uuid;

    // Owned by the mic thread.
    std::unique_ptr<asio::io_context> io;
    std::unique_ptr<udp::socket> socket;
    std::unique_ptr<asio::steady_timer> timer;
    std::thread thread;
    std::atomic<bool> running {false};
    std::unique_ptr<session_t> session;
    std::optional<replaced_t> replaced;
    std::array<char, 2048> receive_buffer;
    udp::endpoint sender;

    void notify_connected(const std::string &name) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_mic_connected(name);
#endif
    }

    void notify_disconnected(const std::string &name, const std::string &reason) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_mic_disconnected(name, reason);
#endif
    }

    void notify_error(protocol::error_e error_code) {
      std::string text;
      switch (error_code) {
        case protocol::error_e::device_missing:
          text = "Apollo cannot find the Steam Streaming Microphone. Install Steam on the PC, then tap Connect.";
          break;
        case protocol::error_e::device_open_failed:
          text = "Another program on the PC is blocking the Steam Streaming Microphone. Close it, then tap Connect.";
          break;
        case protocol::error_e::decode_failed:
          text = "Apollo cannot decode the audio from this device. Tap Disconnect, then tap Connect.";
          break;
        default:
          return;
      }
      BOOST_LOG(warning) << "Remote microphone: "sv << text;
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_mic_error(text);
#endif
    }

    /**
     * @brief Decode the next packet for the virtual microphone. Runs on the render thread.
     * @return The number of samples written to out, or 0 when nothing is ready to play.
     */
    std::size_t fill_from_session(session_t &target, float *out, std::size_t capacity) {
      jitter_buffer_t::pop_result_t next;
      {
        std::lock_guard lock {target.jitter_mutex};
        next = target.jitter.pop();
      }

      if (next.kind == jitter_buffer_t::pop_e::wait) {
        target.waiting = true;
        return 0;
      }
      if (target.waiting) {
        // The sender was muted or silent. Decoding against the state of the last talkspurt
        // blends two unrelated signals, so the decoder starts clean.
        opus_decoder_ctl(target.decoder, OPUS_RESET_STATE);
        target.waiting = false;
      }

      int samples;
      if (next.kind == jitter_buffer_t::pop_e::frame) {
        // capacity covers a 120 ms packet. A 960-sample buffer rejects anything longer than 20 ms.
        samples = opus_decode_float(target.decoder, next.payload.data(), static_cast<opus_int32>(next.payload.size()), out, static_cast<int>(capacity), 0);
      } else {
        // A null payload asks Opus to conceal one lost frame of FRAME_SAMPLES.
        samples = opus_decode_float(target.decoder, nullptr, 0, out, FRAME_SAMPLES, 0);
      }

      if (samples < 0) {
        if (++target.decode_failures == MAX_DECODE_FAILURES) {
          target.error = protocol::error_e::decode_failed;
          notify_error(protocol::error_e::decode_failed);
        }
        return 0;
      }

      target.decode_failures = 0;
      return static_cast<std::size_t>(samples);
    }

    /**
     * @brief End the mic session on the mic thread.
     */
    void end_session(const std::string &reason, bool notify) {
      if (!session) {
        return;
      }

      auto name = session->device.name;
      // Order matters. Destroying the virtual microphone joins the render thread, which is the
      // only user of the decoder. It also restores the default capture device.
      session->vmic.reset();
      if (session->decoder) {
        opus_decoder_destroy(session->decoder);
      }
      session.reset();

      {
        std::lock_guard lock {state_mutex};
        connected_uuid.clear();
        store->previous_default_capture.clear();
        store->save();
      }

      BOOST_LOG(info) << "Remote microphone: session ended for ["sv << name << "] "sv << reason;
      if (notify) {
        notify_disconnected(name, reason);
      }
    }

    std::variant<session_info_t, session_error_e> begin_session(const device_t &device) {
      if (session) {
        // Keep the old key for a short time so the replaced device receives pong code 4.
        replaced = replaced_t {session->id, std::move(session->cipher), steady::now() + REPLACED_LIFETIME};
        // End the old session first. Its destructor restores the default capture device,
        // so the new virtual microphone records the real previous default.
        end_session("replaced by "s + device.name, false);
      }

      auto key_bytes = crypto::rand(16);
      crypto::aes_t key {key_bytes.begin(), key_bytes.end()};
      std::uint32_t id = 0;
      auto id_bytes = crypto::rand(4);
      std::memcpy(&id, id_bytes.data(), sizeof(id));

      auto next = std::make_unique<session_t>();
      next->id = id;
      next->cipher = crypto::cipher::gcm_t {key, false};
      next->device = device;
      next->last_valid = steady::now();

      int opus_error = OPUS_OK;
      next->decoder = opus_decoder_create(48000, 1, &opus_error);
      if (opus_error != OPUS_OK) {
        next->decoder = nullptr;
        next->error = protocol::error_e::decode_failed;
      }

      if (next->decoder) {
        // The callback runs on the render thread until vmic is destroyed, and end_session()
        // destroys vmic before the session, so the raw pointer stays valid.
        auto *target = next.get();
        platf::virtual_mic_error_e vmic_error = platf::virtual_mic_error_e::none;
        next->vmic = platf::virtual_mic(
          [target](float *out, std::size_t capacity) {
            return fill_from_session(*target, out, capacity);
          },
          vmic_error
        );

        if (vmic_error == platf::virtual_mic_error_e::unsupported) {
          opus_decoder_destroy(next->decoder);
          return session_error_e::unsupported;
        }
        if (vmic_error == platf::virtual_mic_error_e::device_missing) {
          next->error = protocol::error_e::device_missing;
        } else if (vmic_error == platf::virtual_mic_error_e::device_open_failed) {
          next->error = protocol::error_e::device_open_failed;
        }
      }

      session = std::move(next);

      {
        std::lock_guard lock {state_mutex};
        connected_uuid = device.uuid;
        if (session->vmic) {
          store->previous_default_capture = session->vmic->previous_default_capture();
          store->save();
        }
      }

      BOOST_LOG(info) << "Remote microphone: session started for ["sv << device.name << ']';
      if (session->error == protocol::error_e::none) {
        notify_connected(device.name);
      } else {
        notify_error(session->error);
      }

      return session_info_t {id, key_bytes, net::map_port(MIC_PORT_OFFSET)};
    }

    void send_pong(crypto::cipher::gcm_t &cipher, std::uint32_t session_id, std::uint32_t sequence, protocol::error_e error_code) {
      const char code = static_cast<char>(error_code);
      auto datagram = protocol::encrypt_packet(cipher, protocol::direction_e::to_client, {protocol::packet_type_e::pong, session_id, sequence}, std::string_view {&code, 1});
      if (datagram.empty()) {
        return;
      }
      boost::system::error_code ec;
      socket->send_to(asio::buffer(datagram), sender, 0, ec);
    }

    void handle_datagram(std::string_view datagram) {
      auto header = protocol::parse_header(datagram);
      if (!header) {
        return;
      }

      if (session && header->session_id == session->id) {
        auto packet = protocol::decrypt_packet(session->cipher, protocol::direction_e::to_host, datagram);
        if (!packet) {
          return;
        }
        session->last_valid = steady::now();

        if (packet->header.type == protocol::packet_type_e::audio) {
          if (session->error == protocol::error_e::none) {
            std::lock_guard lock {session->jitter_mutex};
            session->jitter.push(packet->header.sequence, std::move(packet->payload));
          }
        } else if (packet->header.type == protocol::packet_type_e::ping) {
          send_pong(session->cipher, session->id, packet->header.sequence, session->error);
        }
        return;
      }

      if (replaced && header->session_id == replaced->id && header->type == protocol::packet_type_e::ping) {
        if (protocol::decrypt_packet(replaced->cipher, protocol::direction_e::to_host, datagram)) {
          send_pong(replaced->cipher, replaced->id, header->sequence, protocol::error_e::replaced);
        }
      }
    }

    /**
     * @brief Expire the replaced key, time out a silent session, and check the virtual microphone.
     *
     * One second is enough for these. Playout does not depend on this timer.
     */
    void on_housekeeping(const boost::system::error_code &ec) {
      if (ec) {
        return;
      }

      auto now = steady::now();
      if (replaced && now > replaced->expires) {
        replaced.reset();
      }

      if (session) {
        if (now - session->last_valid > SESSION_TIMEOUT) {
          end_session("connection lost", true);
        } else if (session->vmic && !session->vmic->healthy()) {
          session->error = protocol::error_e::device_open_failed;
          notify_error(protocol::error_e::device_open_failed);
          session->vmic.reset();
        }
      }

      timer->expires_after(HOUSEKEEPING);
      timer->async_wait(on_housekeeping);
    }

    void receive_next() {
      socket->async_receive_from(asio::buffer(receive_buffer), sender, [](const boost::system::error_code &ec, std::size_t bytes) {
        if (ec == asio::error::operation_aborted) {
          return;
        }
        if (!ec) {
          try {
            handle_datagram(std::string_view {receive_buffer.data(), bytes});
          } catch (const std::exception &e) {
            BOOST_LOG(warning) << "Remote microphone: dropped a packet: "sv << e.what();
          }
        }
        receive_next();
      });
    }

    void run(std::promise<bool> ready) {
      try {
        auto address_family = net::af_from_enum_string(config::sunshine.address_family);
        auto protocol_family = address_family == net::IPV4 ? udp::v4() : udp::v6();
        auto port = net::map_port(MIC_PORT_OFFSET);

        socket = std::make_unique<udp::socket>(*io);
        socket->open(protocol_family);
        socket->bind(udp::endpoint(protocol_family, port));

        timer = std::make_unique<asio::steady_timer>(*io);
        timer->expires_after(HOUSEKEEPING);
        timer->async_wait(on_housekeeping);
        receive_next();

        BOOST_LOG(info) << "Remote microphone: listening on UDP port "sv << port;
        running = true;
        ready.set_value(true);
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Remote microphone: could not open the UDP port: "sv << e.what();
        ready.set_value(false);
        return;
      }

      try {
        io->run();
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Remote microphone: the mic thread stopped: "sv << e.what();
      }
      running = false;
    }

    /**
     * @brief Start the mic thread when it is not running. Call with state_mutex held.
     */
    bool ensure_thread() {
      if (running) {
        return true;
      }
      if (thread.joinable()) {
        thread.join();
      }

      io = std::make_unique<asio::io_context>();
      std::promise<bool> ready;
      auto started = ready.get_future();
      thread = std::thread {run, std::move(ready)};
      return started.get();
    }
  }  // namespace

  void start() {
    std::lock_guard lock {state_mutex};

    auto file = std::filesystem::path {config::nvhttp.file_state}.parent_path() / "mic_state.json";
    store = std::make_unique<store_t>(file);
    if (!store->load()) {
      BOOST_LOG(error) << "Remote microphone: could not read "sv << file.string() << ". Paired microphones are unavailable until the file is repaired or deleted."sv;
    }

    if (auto value = protocol::cert_fingerprint(file_handler::read_file(config::nvhttp.cert.c_str()))) {
      fingerprint = *value;
    }

    if (!store->previous_default_capture.empty()) {
      BOOST_LOG(info) << "Remote microphone: restoring the default capture device after an unclean exit"sv;
      platf::restore_default_capture(store->previous_default_capture);
      store->previous_default_capture.clear();
      store->save();
    }

    // Inert until a mic device is paired: no thread and no socket.
    if (!store->devices().empty()) {
      ensure_thread();
    }
  }

  void stop() {
    if (!thread.joinable()) {
      return;
    }
    if (running) {
      asio::post(*io, []() {
        end_session("Apollo is closing", false);
        io->stop();
      });
    }
    thread.join();
  }

  std::optional<std::string> pair_request(const std::string &address, const std::string &name, const std::string &nonce, const crypto::sha256_t &proof) {
    std::optional<std::string> request_id;
    {
      std::lock_guard lock {state_mutex};
      request_id = pairing.request(address, name, nonce, proof, steady::now());
    }
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    if (request_id) {
      system_tray::update_tray_mic_pair_request();
    }
#endif
    return request_id;
  }

  pairing_t::status_t pair_status(const std::string &request_id) {
    std::lock_guard lock {state_mutex};
    return pairing.status(request_id, steady::now());
  }

  std::optional<std::string> submit_pin(const std::string &pin, const std::string &name) {
    std::lock_guard lock {state_mutex};
    auto now = steady::now();
    auto match = pairing.submit_pin(pin, fingerprint, now);
    if (!match) {
      return std::nullopt;
    }

    auto device_name = name.empty() ? match->name : name;
    auto uuid = uuid_util::uuid_t::generate().string();
    auto token = crypto::rand_alphabet(48, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");

    store->add(device_name, uuid, token);
    if (!store->save()) {
      store->remove(uuid);
      return std::nullopt;
    }

    pairing.complete(match->request_id, device_name, uuid, token, now);
    ensure_thread();
    BOOST_LOG(info) << "Remote microphone: paired ["sv << device_name << ']';
    return device_name;
  }

  nlohmann::json list() {
    std::lock_guard lock {state_mutex};
    auto devices = nlohmann::json::array();
    for (const auto &device : store->devices()) {
      devices.push_back({{"name", device.name}, {"uuid", device.uuid}, {"connected", device.uuid == connected_uuid}});
    }
    return devices;
  }

  bool remove(const std::string &uuid) {
    bool was_connected;
    {
      std::lock_guard lock {state_mutex};
      if (!store->remove(uuid)) {
        return false;
      }
      store->save();
      was_connected = connected_uuid == uuid;
    }
    if (was_connected) {
      session_end(uuid);
    }
    return true;
  }

  std::optional<device_t> authorize(const std::string &token) {
    std::lock_guard lock {state_mutex};
    return store->authorize(token);
  }

  std::variant<session_info_t, session_error_e> session_start(const device_t &device) {
    {
      std::lock_guard lock {state_mutex};
      if (!ensure_thread()) {
        return session_error_e::failed;
      }
    }

    std::promise<std::variant<session_info_t, session_error_e>> promise;
    auto result = promise.get_future();
    asio::post(*io, [&]() {
      try {
        promise.set_value(begin_session(device));
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Remote microphone: could not start a session: "sv << e.what();
        promise.set_value(session_error_e::failed);
      }
    });
    return result.get();
  }

  void session_end(const std::string &uuid) {
    if (!running) {
      return;
    }
    asio::post(*io, [uuid]() {
      if (session && session->device.uuid == uuid) {
        end_session("", true);
      }
    });
  }
}  // namespace mic
```

- [ ] **Step 4: Call the module from `main.cpp`**

In `src/main.cpp`, add `#include "mic.h"` after `#include "main.h"`.

Immediately before the line `std::thread httpThread {nvhttp::start};`, add:

```cpp
  // Remote microphone. Inert until a mic device is paired. Started before the config
  // server so that no mic endpoint runs against an unloaded store.
  mic::start();

```

After the line `shutdown_event->view();` and before `httpThread.join();`, add:

```cpp

  mic::stop();
```

- [ ] **Step 5: Add the sources to the Apollo build**

In `cmake/compile_definitions/common.cmake`, after the `mic_pairing.h` line, add:

```cmake
        "${CMAKE_SOURCE_DIR}/src/mic.cpp"
        "${CMAKE_SOURCE_DIR}/src/mic.h"
```

- [ ] **Step 6: Local check**

Run:

```bash
python3 tests/mic_standalone/syntax_check.py src/mic.cpp
python3 tests/mic_standalone/syntax_check.py src/system_tray.cpp
python3 tests/mic_standalone/syntax_check.py src/main.cpp
ninja -C build/mic_standalone && ./build/mic_standalone/test_mic_standalone
```

Expected: three exit codes of 0, then 37 tests PASS. When the syntax check reports that `<opus/opus.h>` is missing, run `brew install opus` and repeat.

- [ ] **Step 7: Confirm the isolation rule**

Run:

```bash
git diff --stat master -- src/stream.cpp src/rtsp.cpp src/nvhttp.cpp src/audio.cpp src/video.cpp
```

Expected: no output.

- [ ] **Step 8: Commit**

```bash
git add src/mic.h src/mic.cpp src/system_tray.h src/system_tray.cpp src/main.cpp cmake/compile_definitions/common.cmake
git commit -m "feat(mic): add mic module with UDP receiver, session, and tray notices"
```

---

### Task 7: Config server routes

**Files:**
- Modify: `src/confighttp.cpp:69-71` (allowlist), near `:183` (helpers), before `start()` (handlers), `:1649` (route table), top of file (include)
- Modify: `docs/api.md` (new section)

**Interfaces:**
- Consumes: the `mic::` functions from Task 6. Existing `send_response`, `bad_request`, `send_unauthorized`, `validateContentType`, `authenticate`, `print_req`, `http::extract_bearer_token`, `SimpleWeb::Crypto::Base64`.
- Produces: the seven endpoints in the spec, with these JSON bodies:
  - `POST /api/mic/pair` request `{name, nonce, proof}` (base64 for `nonce` and `proof`), reply `{status, request_id}`
  - `POST /api/mic/pair/status` request `{request_id}`, reply `{status, state, uuid?, token?, name?}` where `state` is `pending`, `paired`, or `expired`
  - `POST /api/mic/pin` request `{pin, name}`, reply `{status, name?}`
  - `GET /api/mic/list` reply `{status, named_mics: [{name, uuid, connected}]}`
  - `POST /api/mic/remove` request `{uuid}`, reply `{status}`
  - `POST /api/mic/session` reply `{status, session_id, key, port}` (base64 for `key`)
  - `DELETE /api/mic/session` reply `{status}`

- [ ] **Step 1: Add the include and the allowlist entry**

In `src/confighttp.cpp`, add `#include "mic.h"` in alphabetical position among the local includes.

Change the allowlist to:

```cpp
  const std::set<std::string> TOKEN_ALLOWED_PATHS {
    "/api/clients/list",
    "/api/mic/list",
  };
```

- [ ] **Step 2: Add the helpers**

After the closing brace of `checkIPOrigin`, add:

```cpp
  /**
   * @brief Origin rule for the endpoints that Calliope calls without a credential.
   *
   * Accepts localhost and LAN addresses whatever origin_web_ui_allowed holds, because a mic
   * device is always on the LAN. Rejects WAN addresses.
   */
  bool checkMicOrigin(resp_https_t response, req_https_t request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    if (net::from_address(address) > net::LAN) {
      BOOST_LOG(info) << "Remote microphone: ["sv << address << "] -- denied"sv;
      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      return false;
    }
    return true;
  }
```

After the closing brace of `bad_request`, add:

```cpp
  /**
   * @brief Send a JSON error with a specific status code.
   */
  void mic_error(resp_https_t response, SimpleWeb::StatusCode code, const std::string &error_message) {
    nlohmann::json tree;
    tree["status_code"] = static_cast<int>(code);
    tree["status"] = false;
    tree["error"] = error_message;
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(code, tree.dump(), headers);
  }

  /**
   * @brief Authenticate a request by mic token. The token is the sole credential, so the origin check is skipped.
   */
  std::optional<mic::device_t> authenticateMic(resp_https_t response, req_https_t request) {
    auto header = request->header.find("authorization");
    if (header != request->header.end()) {
      if (auto device = mic::authorize(http::extract_bearer_token(header->second))) {
        return device;
      }
    }
    send_unauthorized(response, request);
    return std::nullopt;
  }
```

- [ ] **Step 3: Add the handlers**

Immediately before the definition of `void start()`, add:

```cpp
  /**
   * @brief Start a remote microphone pairing. Called by Calliope.
   *
   * @api_examples{/api/mic/pair| POST| {"name":"iPhone","nonce":"<base64>","proof":"<base64>"}}
   */
  void micPair(resp_https_t response, req_https_t request) {
    if (!checkMicOrigin(response, request) || !validateContentType(response, request, "application/json")) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto input_tree = nlohmann::json::parse(ss.str());
      std::string name = input_tree.value("name", "");
      auto nonce = SimpleWeb::Crypto::Base64::decode(input_tree.value("nonce", ""));
      auto proof_bytes = SimpleWeb::Crypto::Base64::decode(input_tree.value("proof", ""));

      crypto::sha256_t proof;
      if (name.empty() || name.size() > 64 || nonce.size() != 16 || proof_bytes.size() != proof.size()) {
        bad_request(response, request, "Invalid pairing request");
        return;
      }
      std::copy(proof_bytes.begin(), proof_bytes.end(), proof.begin());

      auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
      auto request_id = mic::pair_request(address, name, nonce, proof);
      if (!request_id) {
        mic_error(response, SimpleWeb::StatusCode::client_error_too_many_requests, "Too many pending pairing requests");
        return;
      }

      nlohmann::json output_tree;
      output_tree["status"] = true;
      output_tree["request_id"] = *request_id;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "MicPair: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Poll a remote microphone pairing. Returns the mic token once.
   *
   * @api_examples{/api/mic/pair/status| POST| {"request_id":"<id>"}}
   */
  void micPairStatus(resp_https_t response, req_https_t request) {
    if (!checkMicOrigin(response, request) || !validateContentType(response, request, "application/json")) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto input_tree = nlohmann::json::parse(ss.str());
      auto status = mic::pair_status(input_tree.value("request_id", ""));

      nlohmann::json output_tree;
      output_tree["status"] = true;
      switch (status.state) {
        case mic::pairing_t::state_e::pending:
          output_tree["state"] = "pending";
          break;
        case mic::pairing_t::state_e::paired:
          output_tree["state"] = "paired";
          output_tree["uuid"] = status.uuid;
          output_tree["token"] = status.token;
          output_tree["name"] = status.name;
          break;
        case mic::pairing_t::state_e::expired:
          output_tree["state"] = "expired";
          break;
      }
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "MicPairStatus: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Submit the PIN shown by Calliope. Called by the web UI.
   *
   * @api_examples{/api/mic/pin| POST| {"pin":"1234","name":"Living room iPhone"}}
   */
  void micPin(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto input_tree = nlohmann::json::parse(ss.str());
      auto name = mic::submit_pin(input_tree.value("pin", ""), input_tree.value("name", ""));

      nlohmann::json output_tree;
      output_tree["status"] = name.has_value();
      if (name) {
        output_tree["name"] = *name;
      }
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "MicPin: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief List paired remote microphones. Readable with the read-only API key.
   *
   * @api_examples{/api/mic/list| GET| null}
   */
  void micList(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["named_mics"] = mic::list();
    send_response(response, output_tree);
  }

  /**
   * @brief Remove one paired remote microphone. Ends its mic session when it is connected.
   *
   * @api_examples{/api/mic/remove| POST| {"uuid":"<uuid>"}}
   */
  void micRemove(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto input_tree = nlohmann::json::parse(ss.str());

      nlohmann::json output_tree;
      output_tree["status"] = mic::remove(input_tree.value("uuid", ""));
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "MicRemove: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Start a mic session. Authenticated by mic token.
   *
   * @api_examples{/api/mic/session| POST| null}
   */
  void micSessionStart(resp_https_t response, req_https_t request) {
    auto device = authenticateMic(response, request);
    if (!device) {
      return;
    }

    print_req(request);

    auto result = mic::session_start(*device);
    if (auto error = std::get_if<mic::session_error_e>(&result)) {
      if (*error == mic::session_error_e::unsupported) {
        mic_error(response, SimpleWeb::StatusCode::server_error_not_implemented, "This host cannot receive a microphone");
      } else {
        mic_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, "Could not start a mic session");
      }
      return;
    }

    auto &info = std::get<mic::session_info_t>(result);
    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["session_id"] = info.session_id;
    output_tree["key"] = SimpleWeb::Crypto::Base64::encode(info.key);
    output_tree["port"] = info.port;
    send_response(response, output_tree);
  }

  /**
   * @brief End the mic session of the calling mic device.
   *
   * @api_examples{/api/mic/session| DELETE| null}
   */
  void micSessionEnd(resp_https_t response, req_https_t request) {
    auto device = authenticateMic(response, request);
    if (!device) {
      return;
    }

    print_req(request);

    mic::session_end(device->uuid);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }
```

- [ ] **Step 4: Register the routes**

In `start()`, after the line `server.resource["^/api/token$"]["DELETE"] = revokeApiToken;`, add:

```cpp
    server.resource["^/api/mic/pair$"]["POST"] = micPair;
    server.resource["^/api/mic/pair/status$"]["POST"] = micPairStatus;
    server.resource["^/api/mic/pin$"]["POST"] = micPin;
    server.resource["^/api/mic/list$"]["GET"] = micList;
    server.resource["^/api/mic/remove$"]["POST"] = micRemove;
    server.resource["^/api/mic/session$"]["POST"] = micSessionStart;
    server.resource["^/api/mic/session$"]["DELETE"] = micSessionEnd;
```

- [ ] **Step 5: Document the endpoints**

In `docs/api.md`, before the final navigation table, add:

```markdown
## Remote microphone

Calliope pairs with Apollo as a microphone. See [Remote microphone](remote_microphone.md)
for setup.

| Endpoint | Authentication | Purpose |
|---|---|---|
| `POST /api/mic/pair` | None, LAN only | Calliope starts pairing |
| `POST /api/mic/pair/status` | None, LAN only | Calliope polls for the pairing result |
| `POST /api/mic/pin` | Web UI login | Submits the PIN and device name |
| `GET /api/mic/list` | Web UI login or read-only API key | Lists paired microphones |
| `POST /api/mic/remove` | Web UI login | Removes one microphone |
| `POST /api/mic/session` | Mic token | Starts a mic session |
| `DELETE /api/mic/session` | Mic token | Ends the mic session |

`GET /api/mic/list` returns the same shape as `/api/clients/list`:

    {"status": true, "named_mics": [{"name": "Living room iPhone", "uuid": "...", "connected": true}]}

At most one entry has `connected: true`. The response never contains a token.
```

- [ ] **Step 6: Local check**

Run:

```bash
python3 tests/mic_standalone/syntax_check.py src/confighttp.cpp
```

Expected: exit code 0.

- [ ] **Step 7: Commit**

```bash
git add src/confighttp.cpp docs/api.md
git commit -m "feat(mic): add remote microphone endpoints to the config server"
```

---

### Task 11: Mic module hardening after the Task 6 review

Run this task after Task 7 and before Task 8. It is the fix round for the Task 6 review.

**Files:**
- Create: `src/mic_playout.h`
- Create: `src/mic_playout.cpp`
- Create: `tests/unit/test_mic_playout.cpp`
- Modify: `src/mic.cpp` (replaced in full)
- Modify: `tests/mic_standalone/CMakeLists.txt`
- Modify: `cmake/compile_definitions/common.cmake` (after the `mic_pairing` lines)
- Modify: `tests/unit/test_mic_jitter.cpp`, `test_mic_pairing.cpp`, `test_mic_protocol.cpp`, `test_mic_store.cpp` (formatting only)

**Interfaces:**
- Consumes: `mic::jitter_buffer_t` from Task 2 and Task 10, and everything `src/mic.cpp` consumed in Task 6.
- Produces: `class mic::playout_t` with `bool ready() const`, `bool push(std::uint32_t sequence, std::vector<std::uint8_t> payload)`, `std::size_t fill(float *out, std::size_t capacity)`, `bool decode_failed() const`, `FRAME_SAMPLES = 960`, `MAX_DECODE_FAILURES = 25`.
- `src/mic.h` does not change, so the config server routes from Task 7 are unaffected.

**Findings this task closes.** The Task 6 review found six defects where config server threads meet the mic thread.

1. `stop()` was no latch, so a handler could restart the mic thread after shutdown. A `stopped` flag, set under `state_mutex`, now makes `ensure_thread()` refuse.
2. `stop()` and `ensure_thread()` raced on `thread`. After the latch, only `stop()` touches `thread`.
3. Config threads read `io` with no lock. `io` is now a `shared_ptr`, copied under `state_mutex`, and handlers capture it by value.
4. `ensure_thread()` destroyed the `io_context` while the socket and the timer still referenced it. `run()` now destroys both before it clears `running`.
5. `session_start()` could block forever. The promise is shared and captured by value, the wait has a 30 second limit, and `stop()` destroys the `io_context`, which breaks the promise of a handler that never ran.
6. The render thread raised tray notices. It now only latches a failure inside `playout_t`, and the 1 second housekeeping timer on the mic thread raises the notice once.

Minor findings closed in the same pass:

- The jitter buffer and the decoder move into `playout_t`, which has unit tests against the real Opus decoder.
- `session_t` declares `vmic` last, so member destruction order joins the render thread before the decoder is destroyed. The order no longer depends on a comment.
- A ping with a sequence at or below the last one is a replay and is dropped. Only a fresh packet refreshes the 5 second timeout.
- A failed store write is logged, and `remove()` returns false for it.
- An unreadable host certificate is logged, because pairing cannot succeed without its fingerprint.
- A failed health check clears the persisted previous default capture device.
- The four existing mic test files are formatted with `clang-format`.

- [ ] **Step 1: Write the failing tests**

Create `tests/unit/test_mic_playout.cpp`:

```cpp
/**
 * @file tests/unit/test_mic_playout.cpp
 * @brief Test src/mic_playout.* with the real Opus decoder.
 */
#include <array>
#include <cmath>
#include <gtest/gtest.h>
#include <src/mic_playout.h>

namespace {
  using mic::playout_t;

  // A 20 ms mono Opus frame of silence (CELT, fullband).
  std::vector<std::uint8_t> silence() {
    return {0xF8, 0xFF, 0xFE};
  }

  // Code 3 in the TOC byte requires a frame count byte, so a 1-byte packet is invalid.
  std::vector<std::uint8_t> invalid() {
    return {0x03};
  }

  std::array<float, 5760> buffer;
}  // namespace

TEST(MicPlayout, DecoderIsCreated) {
  playout_t playout;
  EXPECT_TRUE(playout.ready());
  EXPECT_FALSE(playout.decode_failed());
}

TEST(MicPlayout, ReturnsNothingWhileWaiting) {
  playout_t playout;
  EXPECT_EQ(playout.fill(buffer.data(), buffer.size()), 0u);
  playout.push(0, silence());
  EXPECT_EQ(playout.fill(buffer.data(), buffer.size()), 0u);
}

TEST(MicPlayout, DecodesAFrameAfterThePrebuffer) {
  playout_t playout;
  playout.push(0, silence());
  playout.push(1, silence());
  buffer.fill(1.0f);
  ASSERT_EQ(playout.fill(buffer.data(), buffer.size()), 960u);
  for (std::size_t index = 0; index < 960; ++index) {
    ASSERT_LT(std::fabs(buffer[index]), 0.001f) << "sample " << index;
  }
}

TEST(MicPlayout, ConcealsALostFrame) {
  playout_t playout;
  playout.push(0, silence());
  playout.push(2, silence());
  EXPECT_EQ(playout.fill(buffer.data(), buffer.size()), 960u);  // sequence 0
  EXPECT_EQ(playout.fill(buffer.data(), buffer.size()), 960u);  // sequence 1, concealed
  EXPECT_EQ(playout.fill(buffer.data(), buffer.size()), 960u);  // sequence 2
}

TEST(MicPlayout, RejectsADuplicateFrame) {
  playout_t playout;
  EXPECT_TRUE(playout.push(0, silence()));
  EXPECT_FALSE(playout.push(0, silence()));
}

TEST(MicPlayout, SmallOutputBufferReturnsNothing) {
  playout_t playout;
  playout.push(0, silence());
  playout.push(1, silence());
  EXPECT_EQ(playout.fill(buffer.data(), 959), 0u);
}

TEST(MicPlayout, LatchesAfterRepeatedDecodeFailures) {
  playout_t playout;
  std::uint32_t sequence = 0;
  playout.push(sequence++, invalid());
  for (int call = 0; call < playout_t::MAX_DECODE_FAILURES; ++call) {
    playout.push(sequence++, invalid());
    EXPECT_FALSE(playout.decode_failed()) << "call " << call;
    EXPECT_EQ(playout.fill(buffer.data(), buffer.size()), 0u);
  }
  EXPECT_TRUE(playout.decode_failed());
}

TEST(MicPlayout, AGoodFrameClearsTheFailureCount) {
  playout_t playout;
  std::uint32_t sequence = 0;
  // Each call queues one frame and decodes the frame queued before it.
  auto feed = [&](std::vector<std::uint8_t> payload) {
    playout.push(sequence++, std::move(payload));
    return playout.fill(buffer.data(), buffer.size());
  };

  playout.push(sequence++, invalid());
  for (int call = 0; call < playout_t::MAX_DECODE_FAILURES - 2; ++call) {
    feed(invalid());
  }
  feed(silence());  // decodes the last invalid frame: one failure short of the limit
  EXPECT_EQ(feed(silence()), 960u);  // a good frame
  for (int call = 0; call < playout_t::MAX_DECODE_FAILURES - 1; ++call) {
    feed(invalid());
  }
  EXPECT_FALSE(playout.decode_failed());
}
```

- [ ] **Step 2: Create the header**

Create `src/mic_playout.h`:

```cpp
/**
 * @file src/mic_playout.h
 * @brief Jitter buffer plus Opus decoder for one mic session.
 */
#pragma once

// standard includes
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// local includes
#include "mic_jitter.h"

struct OpusDecoder;

namespace mic {
  /**
   * @brief Turns queued Opus frames into 48 kHz mono float audio.
   *
   * push() runs on the network thread. fill() runs on the audio render thread and is the
   * only user of the decoder. One mutex guards the jitter buffer between the two.
   */
  class playout_t {
  public:
    static constexpr int FRAME_SAMPLES = 960;  // 20 ms at 48 kHz, the duration concealed for one lost frame
    static constexpr int MAX_DECODE_FAILURES = 25;  // 500 ms of consecutive failures

    playout_t();
    ~playout_t();

    playout_t(const playout_t &) = delete;
    playout_t &operator=(const playout_t &) = delete;

    /**
     * @return false when the Opus decoder could not be created.
     */
    bool ready() const {
      return decoder != nullptr;
    }

    /**
     * @brief Queue one Opus frame.
     * @return false for a duplicate or late frame.
     */
    bool push(std::uint32_t sequence, std::vector<std::uint8_t> payload);

    /**
     * @brief Decode the next frame into out.
     * @param capacity Samples available in out. Use platf::VIRTUAL_MIC_MAX_PACKET_SAMPLES.
     * @return The number of samples written, or 0 when nothing is ready to play.
     */
    std::size_t fill(float *out, std::size_t capacity);

    /**
     * @return true after MAX_DECODE_FAILURES consecutive decode failures. Safe from any thread.
     */
    bool decode_failed() const {
      return failed;
    }

  private:
    std::mutex jitter_mutex;
    jitter_buffer_t jitter;

    // Touched only by the thread that calls fill().
    OpusDecoder *decoder = nullptr;
    bool waiting = true;
    int decode_failures = 0;

    std::atomic<bool> failed {false};
  };
}  // namespace mic
```

- [ ] **Step 3: Add libopus to the standalone target**

In `tests/mic_standalone/CMakeLists.txt`:

After the line `find_package(OpenSSL REQUIRED)`, add:

```cmake

# libopus, for the playout tests. Apollo includes it as <opus/opus.h>.
find_path(OPUS_INCLUDE_DIR opus/opus.h REQUIRED)
find_library(OPUS_LIBRARY opus REQUIRED)
```

Change the `foreach` line to:

```cmake
foreach(unit mic_protocol mic_jitter mic_store mic_pairing mic_playout)
```

Change the `target_include_directories` line to:

```cmake
target_include_directories(${PROJECT_NAME} PRIVATE "${APOLLO_DIR}" "${OPUS_INCLUDE_DIR}")
```

Change the `target_link_libraries` call to:

```cmake
target_link_libraries(${PROJECT_NAME} PRIVATE
        OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json "${OPUS_LIBRARY}" gtest gtest_main)
```

- [ ] **Step 4: Run the tests and confirm they fail**

The build directory can hold a cache from another generator. Configure a fresh one:

```bash
rm -rf build/mic_standalone
cmake -S tests/mic_standalone -B build/mic_standalone -G Ninja -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
ninja -C build/mic_standalone
```

Expected: the build FAILS at link time with undefined `mic::playout_t::playout_t`.

- [ ] **Step 5: Write the implementation**

Create `src/mic_playout.cpp`:

```cpp
/**
 * @file src/mic_playout.cpp
 * @brief Definitions for remote microphone playout.
 */
// lib includes
#include <opus/opus.h>

// local includes
#include "mic_playout.h"

namespace mic {
  playout_t::playout_t() {
    int status = OPUS_OK;
    decoder = opus_decoder_create(48000, 1, &status);
    if (status != OPUS_OK) {
      decoder = nullptr;
    }
  }

  playout_t::~playout_t() {
    if (decoder) {
      opus_decoder_destroy(decoder);
    }
  }

  bool playout_t::push(std::uint32_t sequence, std::vector<std::uint8_t> payload) {
    std::lock_guard lock {jitter_mutex};
    return jitter.push(sequence, std::move(payload));
  }

  std::size_t playout_t::fill(float *out, std::size_t capacity) {
    if (!decoder || capacity < static_cast<std::size_t>(FRAME_SAMPLES)) {
      return 0;
    }

    jitter_buffer_t::pop_result_t next;
    {
      std::lock_guard lock {jitter_mutex};
      next = jitter.pop();
    }

    if (next.kind == jitter_buffer_t::pop_e::wait) {
      waiting = true;
      return 0;
    }
    if (waiting) {
      // The sender was muted or silent. Decoding against the state of the last talkspurt
      // blends two unrelated signals, so the decoder starts clean.
      opus_decoder_ctl(decoder, OPUS_RESET_STATE);
      waiting = false;
    }

    int samples;
    if (next.kind == jitter_buffer_t::pop_e::frame && !next.payload.empty()) {
      samples = opus_decode_float(decoder, next.payload.data(), static_cast<opus_int32>(next.payload.size()), out, static_cast<int>(capacity), 0);
    } else {
      // A null payload asks Opus to conceal one lost frame of FRAME_SAMPLES.
      samples = opus_decode_float(decoder, nullptr, 0, out, FRAME_SAMPLES, 0);
    }

    if (samples < 0) {
      if (++decode_failures >= MAX_DECODE_FAILURES) {
        failed = true;
      }
      return 0;
    }

    decode_failures = 0;
    return static_cast<std::size_t>(samples);
  }
}  // namespace mic
```

- [ ] **Step 6: Run the tests and confirm they pass**

Run:

```bash
cmake -S tests/mic_standalone -B build/mic_standalone && ninja -C build/mic_standalone && ./build/mic_standalone/test_mic_standalone
```

Expected: 45 tests from 5 suites, all PASS. `MicPlayout` has 8 tests.

- [ ] **Step 7: Replace `src/mic.cpp`**

Replace the whole content of `src/mic.cpp` with:

```cpp
/**
 * @file src/mic.cpp
 * @brief Definitions for the remote microphone module.
 *
 * This module never touches streaming code. Every failure ends the mic session only.
 *
 * Playout is pull-based: the virtual microphone's render thread calls playout_t::fill()
 * when the device has room, so the audio device clock is the only playout clock.
 * Design evidence: docs/superpowers/research/2026-09-20-voice-playout.md.
 *
 * Threads:
 * - The mic thread runs one io_context. It owns the socket, the timer, and the mic session.
 * - The render thread belongs to the virtual microphone. It touches playout_t only.
 * - Config server threads call the functions in mic.h. They reach the mic session by
 *   posting to the io_context. state_mutex guards everything they share with the mic
 *   thread: the store, the pairing state, the connected device id, and the lifecycle
 *   (io, thread, stopped).
 */
// standard includes
#include <array>
#include <atomic>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "config.h"
#include "file_handler.h"
#include "logging.h"
#include "mic.h"
#include "mic_playout.h"
#include "mic_protocol.h"
#include "network.h"
#include "platform/common.h"
#include "system_tray.h"
#include "uuid.h"

using namespace std::literals;

namespace mic {
  namespace {
    namespace asio = boost::asio;
    using asio::ip::udp;
    using steady = std::chrono::steady_clock;

    constexpr int MIC_PORT_OFFSET = 13;
    constexpr auto HOUSEKEEPING = 1s;
    constexpr auto SESSION_TIMEOUT = 5s;
    constexpr auto REPLACED_LIFETIME = 5s;
    // Opening the virtual microphone can install a driver, which takes several seconds.
    constexpr auto SESSION_START_TIMEOUT = 30s;

    struct session_t {
      std::uint32_t id;
      crypto::cipher::gcm_t cipher;
      device_t device;
      steady::time_point last_valid;

      // Pings count from 0 and only ever increase, so an old ping is a replay.
      bool have_ping = false;
      std::uint32_t last_ping = 0;

      protocol::error_e error_code = protocol::error_e::none;  ///< Sent in every pong
      protocol::error_e notified = protocol::error_e::none;  ///< Last error shown in the tray

      // Members die in reverse order. vmic is declared last, so the virtual microphone and its
      // render thread are gone before playout, which holds the decoder that thread uses.
      playout_t playout;
      std::unique_ptr<platf::virtual_mic_t> vmic;
    };

    struct replaced_t {
      std::uint32_t id;
      crypto::cipher::gcm_t cipher;
      steady::time_point expires;
    };

    // Guarded by state_mutex. Shared by config server threads and the mic thread.
    std::mutex state_mutex;
    std::unique_ptr<store_t> store;
    pairing_t pairing;
    crypto::sha256_t fingerprint {};
    std::string connected_uuid;
    std::shared_ptr<asio::io_context> io;
    std::thread thread;
    bool stopped = false;  ///< Set once by stop(). The mic thread never starts again.

    std::atomic<bool> running {false};

    // Owned by the mic thread. Created and destroyed inside run().
    std::unique_ptr<udp::socket> socket;
    std::unique_ptr<asio::steady_timer> timer;
    std::unique_ptr<session_t> session;
    std::optional<replaced_t> replaced;
    std::array<char, 2048> receive_buffer;
    udp::endpoint sender;

    void notify_connected(const std::string &name) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_mic_connected(name);
#endif
    }

    void notify_disconnected(const std::string &name, const std::string &reason) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_mic_disconnected(name, reason);
#endif
    }

    void notify_error(protocol::error_e error_code) {
      std::string text;
      switch (error_code) {
        case protocol::error_e::device_missing:
          text = "Apollo cannot find the Steam Streaming Microphone. Install Steam on the PC, then tap Connect.";
          break;
        case protocol::error_e::device_open_failed:
          text = "Another program on the PC is blocking the Steam Streaming Microphone. Close it, then tap Connect.";
          break;
        case protocol::error_e::decode_failed:
          text = "Apollo cannot decode the audio from this device. Tap Disconnect, then tap Connect.";
          break;
        default:
          return;
      }
      BOOST_LOG(warning) << "Remote microphone: "sv << text;
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_mic_error(text);
#endif
    }

    /**
     * @brief Write the store and log a failure. Call with state_mutex held.
     */
    bool save_store() {
      if (store->save()) {
        return true;
      }
      BOOST_LOG(error) << "Remote microphone: could not write mic_state.json"sv;
      return false;
    }

    /**
     * @brief Show a session error in the tray once. Runs on the mic thread only.
     */
    void report_error(session_t &target) {
      if (target.error_code != target.notified) {
        notify_error(target.error_code);
        target.notified = target.error_code;
      }
    }

    /**
     * @brief End the mic session on the mic thread.
     */
    void end_session(const std::string &reason, bool notify) {
      if (!session) {
        return;
      }

      auto name = session->device.name;
      // Destroying the session destroys the virtual microphone first (see session_t), which
      // joins the render thread and restores the default capture device.
      session.reset();

      {
        std::lock_guard lock {state_mutex};
        connected_uuid.clear();
        store->previous_default_capture.clear();
        save_store();
      }

      BOOST_LOG(info) << "Remote microphone: session ended for ["sv << name << "] "sv << reason;
      if (notify) {
        notify_disconnected(name, reason);
      }
    }

    std::variant<session_info_t, session_error_e> begin_session(const device_t &device) {
      if (session) {
        // Keep the old key for a short time so the replaced device receives pong code 4.
        replaced = replaced_t {session->id, std::move(session->cipher), steady::now() + REPLACED_LIFETIME};
        // End the old session first. Its destructor restores the default capture device,
        // so the new virtual microphone records the real previous default.
        end_session("replaced by "s + device.name, false);
      }

      auto key_bytes = crypto::rand(16);
      crypto::aes_t key {key_bytes.begin(), key_bytes.end()};
      std::uint32_t id = 0;
      auto id_bytes = crypto::rand(4);
      std::memcpy(&id, id_bytes.data(), sizeof(id));

      auto next = std::make_unique<session_t>();
      next->id = id;
      next->cipher = crypto::cipher::gcm_t {key, false};
      next->device = device;
      next->last_valid = steady::now();

      if (!next->playout.ready()) {
        next->error_code = protocol::error_e::decode_failed;
      } else {
        // The callback runs on the render thread until vmic is destroyed. vmic is the last
        // member of session_t, so it is destroyed before playout.
        auto *playout = &next->playout;
        platf::virtual_mic_error_e vmic_error = platf::virtual_mic_error_e::none;
        next->vmic = platf::virtual_mic(
          [playout](float *out, std::size_t capacity) {
            return playout->fill(out, capacity);
          },
          vmic_error
        );

        if (vmic_error == platf::virtual_mic_error_e::unsupported) {
          return session_error_e::unsupported;
        }
        if (vmic_error == platf::virtual_mic_error_e::device_missing) {
          next->error_code = protocol::error_e::device_missing;
        } else if (vmic_error == platf::virtual_mic_error_e::device_open_failed) {
          next->error_code = protocol::error_e::device_open_failed;
        }
      }

      session = std::move(next);

      {
        std::lock_guard lock {state_mutex};
        connected_uuid = device.uuid;
        if (session->vmic) {
          store->previous_default_capture = session->vmic->previous_default_capture();
          save_store();
        }
      }

      BOOST_LOG(info) << "Remote microphone: session started for ["sv << device.name << ']';
      if (session->error_code == protocol::error_e::none) {
        notify_connected(device.name);
      } else {
        report_error(*session);
      }

      return session_info_t {id, key_bytes, net::map_port(MIC_PORT_OFFSET)};
    }

    void send_pong(crypto::cipher::gcm_t &cipher, std::uint32_t session_id, std::uint32_t sequence, protocol::error_e error_code) {
      const char code = static_cast<char>(error_code);
      auto datagram = protocol::encrypt_packet(cipher, protocol::direction_e::to_client, {protocol::packet_type_e::pong, session_id, sequence}, std::string_view {&code, 1});
      if (datagram.empty()) {
        return;
      }
      boost::system::error_code ec;
      socket->send_to(asio::buffer(datagram), sender, 0, ec);
    }

    void handle_datagram(std::string_view datagram) {
      auto header = protocol::parse_header(datagram);
      if (!header) {
        return;
      }

      if (session && header->session_id == session->id) {
        auto packet = protocol::decrypt_packet(session->cipher, protocol::direction_e::to_host, datagram);
        if (!packet) {
          return;
        }

        // Only a fresh packet keeps the session alive. A captured packet sent again must not.
        if (packet->header.type == protocol::packet_type_e::audio) {
          if (session->error_code == protocol::error_e::none && session->playout.push(packet->header.sequence, std::move(packet->payload))) {
            session->last_valid = steady::now();
          }
        } else if (packet->header.type == protocol::packet_type_e::ping) {
          if (session->have_ping && packet->header.sequence <= session->last_ping) {
            return;
          }
          session->have_ping = true;
          session->last_ping = packet->header.sequence;
          session->last_valid = steady::now();
          send_pong(session->cipher, session->id, packet->header.sequence, session->error_code);
        }
        return;
      }

      if (replaced && header->session_id == replaced->id && header->type == protocol::packet_type_e::ping) {
        if (protocol::decrypt_packet(replaced->cipher, protocol::direction_e::to_host, datagram)) {
          send_pong(replaced->cipher, replaced->id, header->sequence, protocol::error_e::replaced);
        }
      }
    }

    /**
     * @brief Expire the replaced key, time out a silent session, and check the virtual microphone.
     *
     * One second is enough for these. Playout does not depend on this timer.
     */
    void on_housekeeping(const boost::system::error_code &ec) {
      if (ec) {
        return;
      }

      auto now = steady::now();
      if (replaced && now > replaced->expires) {
        replaced.reset();
      }

      if (session) {
        if (now - session->last_valid > SESSION_TIMEOUT) {
          end_session("connection lost", true);
        } else {
          // The render thread only latches a failure. The tray is driven from this thread.
          if (session->error_code == protocol::error_e::none && session->playout.decode_failed()) {
            session->error_code = protocol::error_e::decode_failed;
          }
          if (session->vmic && !session->vmic->healthy()) {
            session->error_code = protocol::error_e::device_open_failed;
            session->vmic.reset();  // restores the default capture device

            std::lock_guard lock {state_mutex};
            store->previous_default_capture.clear();
            save_store();
          }
          report_error(*session);
        }
      }

      timer->expires_after(HOUSEKEEPING);
      timer->async_wait(on_housekeeping);
    }

    void receive_next() {
      socket->async_receive_from(asio::buffer(receive_buffer), sender, [](const boost::system::error_code &ec, std::size_t bytes) {
        if (ec == asio::error::operation_aborted) {
          return;
        }
        if (!ec) {
          try {
            handle_datagram(std::string_view {receive_buffer.data(), bytes});
          } catch (const std::exception &e) {
            BOOST_LOG(warning) << "Remote microphone: dropped a packet: "sv << e.what();
          }
        }
        receive_next();
      });
    }

    /**
     * @brief The mic thread. Holds its own reference to the io_context for its whole life.
     */
    void run(std::shared_ptr<asio::io_context> context, std::promise<bool> ready) {
      try {
        auto address_family = net::af_from_enum_string(config::sunshine.address_family);
        auto protocol_family = address_family == net::IPV4 ? udp::v4() : udp::v6();
        auto port = net::map_port(MIC_PORT_OFFSET);

        socket = std::make_unique<udp::socket>(*context);
        socket->open(protocol_family);
        socket->bind(udp::endpoint(protocol_family, port));

        timer = std::make_unique<asio::steady_timer>(*context);
        timer->expires_after(HOUSEKEEPING);
        timer->async_wait(on_housekeeping);
        receive_next();

        BOOST_LOG(info) << "Remote microphone: listening on UDP port "sv << port;
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Remote microphone: could not open the UDP port: "sv << e.what();
        // I/O objects must die before their io_context.
        timer.reset();
        socket.reset();
        ready.set_value(false);
        return;
      }

      running = true;
      ready.set_value(true);

      try {
        context->run();
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Remote microphone: the mic thread stopped: "sv << e.what();
      }

      // A session must not outlive its thread: nothing would time it out or restore the device.
      end_session("the mic thread stopped", false);
      timer.reset();
      socket.reset();
      // Last statement. ensure_thread() joins this thread while it holds state_mutex, and
      // end_session() above takes state_mutex, so running stays true until that is done.
      running = false;
    }

    /**
     * @brief Start the mic thread when it is not running. Call with state_mutex held.
     * @return false after stop(), or when the UDP port cannot be opened.
     */
    bool ensure_thread() {
      if (stopped) {
        return false;
      }
      if (running) {
        return true;
      }
      if (thread.joinable()) {
        thread.join();
      }

      // The previous thread destroyed its socket and timer before it cleared running.
      io = std::make_shared<asio::io_context>();
      std::promise<bool> ready;
      auto started = ready.get_future();
      thread = std::thread {run, io, std::move(ready)};
      return started.get();
    }

    /**
     * @brief The io_context to post to, or null when the mic thread is not running.
     */
    std::shared_ptr<asio::io_context> live_context() {
      std::lock_guard lock {state_mutex};
      if (stopped || !running) {
        return nullptr;
      }
      return io;
    }
  }  // namespace

  void start() {
    std::lock_guard lock {state_mutex};

    auto file = std::filesystem::path {config::nvhttp.file_state}.parent_path() / "mic_state.json";
    store = std::make_unique<store_t>(file);
    if (!store->load()) {
      BOOST_LOG(error) << "Remote microphone: could not read "sv << file.string() << ". No microphone is paired. The next pairing replaces the file."sv;
    }

    if (auto value = protocol::cert_fingerprint(file_handler::read_file(config::nvhttp.cert.c_str()))) {
      fingerprint = *value;
    } else {
      BOOST_LOG(error) << "Remote microphone: could not read the host certificate. Pairing a microphone fails until Apollo restarts with a readable certificate."sv;
    }

    if (!store->previous_default_capture.empty()) {
      BOOST_LOG(info) << "Remote microphone: restoring the default capture device after an unclean exit"sv;
      platf::restore_default_capture(store->previous_default_capture);
      store->previous_default_capture.clear();
      save_store();
    }

    // Inert until a mic device is paired: no thread and no socket.
    if (!store->devices().empty()) {
      ensure_thread();
    }
  }

  void stop() {
    std::shared_ptr<asio::io_context> context;
    {
      std::lock_guard lock {state_mutex};
      stopped = true;  // from here on ensure_thread() refuses, so nobody else touches thread
      context = io;
    }

    if (context && running) {
      asio::post(*context, [context]() {
        end_session("Apollo is closing", false);
        context->stop();
      });
    }
    if (thread.joinable()) {
      thread.join();
    }

    // Dropping the last references destroys the io_context and every handler still queued in
    // it. That breaks the promise of a session_start() whose handler never ran.
    context.reset();
    std::lock_guard lock {state_mutex};
    io.reset();
  }

  std::optional<std::string> pair_request(const std::string &address, const std::string &name, const std::string &nonce, const crypto::sha256_t &proof) {
    std::optional<std::string> request_id;
    {
      std::lock_guard lock {state_mutex};
      request_id = pairing.request(address, name, nonce, proof, steady::now());
    }
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    if (request_id) {
      system_tray::update_tray_mic_pair_request();
    }
#endif
    return request_id;
  }

  pairing_t::status_t pair_status(const std::string &request_id) {
    std::lock_guard lock {state_mutex};
    return pairing.status(request_id, steady::now());
  }

  std::optional<std::string> submit_pin(const std::string &pin, const std::string &name) {
    std::lock_guard lock {state_mutex};
    auto now = steady::now();
    auto match = pairing.submit_pin(pin, fingerprint, now);
    if (!match) {
      return std::nullopt;
    }

    auto device_name = name.empty() ? match->name : name;
    auto uuid = uuid_util::uuid_t::generate().string();
    auto token = crypto::rand_alphabet(48, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");

    store->add(device_name, uuid, token);
    if (!save_store()) {
      store->remove(uuid);
      return std::nullopt;
    }

    pairing.complete(match->request_id, device_name, uuid, token, now);
    ensure_thread();
    BOOST_LOG(info) << "Remote microphone: paired ["sv << device_name << ']';
    return device_name;
  }

  nlohmann::json list() {
    std::lock_guard lock {state_mutex};
    auto devices = nlohmann::json::array();
    for (const auto &device : store->devices()) {
      devices.push_back({{"name", device.name}, {"uuid", device.uuid}, {"connected", device.uuid == connected_uuid}});
    }
    return devices;
  }

  bool remove(const std::string &uuid) {
    bool was_connected;
    bool saved;
    {
      std::lock_guard lock {state_mutex};
      if (!store->remove(uuid)) {
        return false;
      }
      saved = save_store();
      was_connected = connected_uuid == uuid;
    }
    if (was_connected) {
      session_end(uuid);
    }
    // false tells the web UI that the device returns after a restart.
    return saved;
  }

  std::optional<device_t> authorize(const std::string &token) {
    std::lock_guard lock {state_mutex};
    return store->authorize(token);
  }

  std::variant<session_info_t, session_error_e> session_start(const device_t &device) {
    using result_t = std::variant<session_info_t, session_error_e>;

    std::shared_ptr<asio::io_context> context;
    {
      std::lock_guard lock {state_mutex};
      if (!ensure_thread()) {
        return session_error_e::failed;
      }
      context = io;
    }

    // Shared, and captured by value: the handler can run after this function gave up waiting.
    auto promise = std::make_shared<std::promise<result_t>>();
    auto result = promise->get_future();
    asio::post(*context, [promise, device]() {
      try {
        promise->set_value(begin_session(device));
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Remote microphone: could not start a session: "sv << e.what();
        promise->set_value(session_error_e::failed);
      }
    });
    // stop() destroys the io_context to break this promise when the handler never runs.
    context.reset();

    try {
      if (result.wait_for(SESSION_START_TIMEOUT) != std::future_status::ready) {
        return session_error_e::failed;
      }
      return result.get();
    } catch (const std::future_error &) {
      return session_error_e::failed;
    }
  }

  void session_end(const std::string &uuid) {
    auto context = live_context();
    if (!context) {
      return;
    }
    asio::post(*context, [uuid]() {
      if (session && session->device.uuid == uuid) {
        end_session("", true);
      }
    });
  }
}  // namespace mic
```

- [ ] **Step 8: Add the new sources to the Apollo build**

In `cmake/compile_definitions/common.cmake`, after the `mic_pairing.h` line, add:

```cmake
        "${CMAKE_SOURCE_DIR}/src/mic_playout.cpp"
        "${CMAKE_SOURCE_DIR}/src/mic_playout.h"
```

- [ ] **Step 9: Format the mic test files**

Run:

```bash
xcrun clang-format -i tests/unit/test_mic_jitter.cpp tests/unit/test_mic_pairing.cpp tests/unit/test_mic_protocol.cpp tests/unit/test_mic_store.cpp tests/unit/test_mic_playout.cpp
```

The change is limited to include order and line wrapping.

- [ ] **Step 10: Local checks**

Run:

```bash
python3 tests/mic_standalone/syntax_check.py src/mic.cpp ; echo "exit: $?"
python3 tests/mic_standalone/syntax_check.py src/confighttp.cpp ; echo "exit: $?"
ninja -C build/mic_standalone && ./build/mic_standalone/test_mic_standalone
for f in src/mic*.h src/mic*.cpp tests/unit/test_mic_*.cpp; do echo "$(xcrun clang-format "$f" | diff - "$f" | grep -c '^[<>]') $f"; done
git diff --stat master -- src/stream.cpp src/rtsp.cpp src/nvhttp.cpp src/audio.cpp src/video.cpp
```

Expected: two exit codes of 0, 45 tests PASS, a count of 0 beside every file, and no output from the last command.

- [ ] **Step 11: Commit**

```bash
git add src/mic_playout.h src/mic_playout.cpp src/mic.cpp tests/unit/test_mic_playout.cpp tests/unit/test_mic_jitter.cpp tests/unit/test_mic_pairing.cpp tests/unit/test_mic_protocol.cpp tests/unit/test_mic_store.cpp tests/mic_standalone/CMakeLists.txt cmake/compile_definitions/common.cmake
git commit -m "fix(mic): make the mic thread lifecycle safe and extract tested playout"
```

---

### Task 8: Microphone tab on the PIN page

**Files:**
- Modify: `src_assets/common/assets/web/pin.html:22-36` (tabs and forms), `:199-207` (list), `:357-375` (data), `:397-400` (created), methods block
- Modify: `src_assets/common/assets/web/public/assets/locale/en.json` (the `pin` object)

**Interfaces:**
- Consumes: `POST /api/mic/pin`, `GET /api/mic/list`, `POST /api/mic/remove` from Task 7.
- Produces: the `#MIC` tab that the tray notice from Task 6 opens at `/pin#MIC`.

- [ ] **Step 1: Add the strings**

In `en.json`, inside the `"pin"` object, add these keys. Keep the object in its existing order and append the keys at the end:

```json
    "mic_pairing": "Microphone",
    "mic_intro": "Pair a phone or Mac running Calliope as a microphone for this PC. Open Calliope, choose this PC, and enter the PIN it shows.",
    "mic_pair_success": "{name} is paired as a microphone. Tap Connect in Calliope to start. While connected, Apollo makes it the default microphone on this PC.",
    "mic_pair_failure": "Pairing failed. Check the PIN in Calliope. After three wrong PINs, tap Get a new PIN in Calliope.",
    "mic_devices": "Microphones",
    "mic_devices_desc": "Phones and Macs paired through Calliope. These are separate from the streaming devices above.",
    "mic_no_devices": "There are no paired microphones.",
    "mic_connected": "Connected",
    "mic_remove_confirm": "Remove {name}? It will disconnect now and must be paired again to reconnect."
```

- [ ] **Step 2: Add the tab and the form**

Replace the tab list (the `<ul class="nav nav-pills pin-tab-bar ...">` element) with:

```html
    <ul class="nav nav-pills pin-tab-bar justify-content-center">
      <li class="nav-item">
        <a class="nav-link" :class="{active: currentTab !== '#PIN' && currentTab !== '#MIC'}" href="#OTP" @click.prevent="switchTab('OTP')">{{ $t('pin.otp_pairing') }}</a>
      </li>
      <li class="nav-item">
        <a class="nav-link" :class="{active: currentTab === '#PIN'}" href="#PIN" @click.prevent="switchTab('PIN')">{{ $t('pin.pin_pairing') }}</a>
      </li>
      <li class="nav-item">
        <a class="nav-link" :class="{active: currentTab === '#MIC'}" href="#MIC" @click.prevent="switchTab('MIC')">{{ $t('pin.mic_pairing') }}</a>
      </li>
    </ul>
```

Directly after the closing `</form>` of the `#PIN` form, and before the `<form v-else ...>` of the OTP form, add:

```html
    <form v-else-if="currentTab === '#MIC'" class="form d-flex flex-column align-items-center" @submit.prevent="registerMic">
      <div class="card flex-column d-flex p-4 mb-4">
        <p class="text-center">{{ $t('pin.mic_intro') }}</p>
        <input type="text" pattern="\d*" :placeholder="`${$t('navbar.pin')}`" v-model="micPin" autofocus class="form-control mt-2" required />
        <input type="text" :placeholder="`${$t('pin.device_name')}`" v-model="micName" class="form-control my-4" />
        <button class="btn btn-primary">{{ $t('pin.send') }}</button>
      </div>
      <div v-if="micMessage" class="alert" :class="['alert-' + micStatus]" role="alert">{{ micMessage }}</div>
    </form>
```

- [ ] **Step 3: Add the mic device list**

After the closing `</div>` of the "Manage Clients" card and before `</div>` of `#content`, add:

```html
    <!-- Manage microphones -->
    <div class="card my-4 align-self-stretch">
      <div class="card-body">
        <div class="p-2">
          <h2 class="me-auto">{{ $t('pin.mic_devices') }}</h2>
          <p class="mt-3">{{ $t('pin.mic_devices_desc') }}</p>
        </div>
      </div>
      <ul class="list-group list-group-flush list-group-item-light" v-if="mics.length > 0">
        <div v-for="mic in mics" :key="mic.uuid" class="list-group-item d-flex align-items-center p-3">
          <span class="me-2">{{ mic.name }}</span>
          <span v-if="mic.connected" class="badge bg-success me-auto">{{ $t('pin.mic_connected') }}</span>
          <span v-else class="me-auto"></span>
          <div class="me-2 btn btn-danger" @click="removeMic(mic)"><i class="fas fa-trash"></i></div>
        </div>
      </ul>
      <ul v-else class="list-group list-group-flush list-group-item-light">
        <div class="list-group-item p-3 text-center"><em>{{ $t('pin.mic_no_devices') }}</em></div>
      </ul>
    </div>
```

- [ ] **Step 4: Add the data and methods**

In the `data` function, after `clients: [],`, add:

```js
      mics: [],
      micPin: '',
      micName: '',
      micMessage: '',
      micStatus: 'success',
```

Change `switchTab` so the mic list survives a tab change:

```js
      switchTab(currentTab) {
        location.hash = currentTab;
        const clients = this.clients;
        const mics = this.mics;
        Object.assign(this, data(), { clients, mics });
        hostInfoCache = null;
        clearTimeout(resetOTPTimeout);
      },
```

In `created()`, after `this.refreshClients();`, add:

```js
      this.refreshMics();
```

In `methods`, after `switchTab`, add:

```js
      registerMic() {
        this.micMessage = '';
        fetch("./api/mic/pin", {
          credentials: 'include',
          headers: { 'Content-Type': 'application/json' },
          method: 'POST',
          body: JSON.stringify({ pin: this.micPin, name: this.micName })
        })
          .then((response) => response.json())
          .then((response) => {
            if (response.status === true) {
              this.micStatus = 'success';
              this.micMessage = this.i18n.t('pin.mic_pair_success', { name: response.name });
              this.micPin = '';
              this.micName = '';
              this.refreshMics();
            } else {
              this.micStatus = 'danger';
              this.micMessage = this.i18n.t('pin.mic_pair_failure');
            }
          });
      },
      removeMic(mic) {
        if (!confirm(this.i18n.t('pin.mic_remove_confirm', { name: mic.name }))) return;
        fetch("./api/mic/remove", {
          credentials: 'include',
          headers: { 'Content-Type': 'application/json' },
          method: 'POST',
          body: JSON.stringify({ uuid: mic.uuid })
        }).then(() => this.refreshMics());
      },
      refreshMics() {
        fetch("./api/mic/list", { credentials: 'include' })
          .then((response) => response.json())
          .then((response) => {
            this.mics = response.status && response.named_mics ? response.named_mics : [];
          });
      },
```

- [ ] **Step 5: Build the web UI**

Run:

```bash
npm install && npm run build
```

Expected: the build completes with no error, and `build/assets/web/pin.html` exists.

- [ ] **Step 6: Commit**

```bash
git add src_assets/common/assets/web/pin.html src_assets/common/assets/web/public/assets/locale/en.json
git commit -m "feat(mic): add Microphone tab and mic device list to the PIN page"
```

---

### Task 9: User documentation and Windows verification

**Files:**
- Create: `docs/remote_microphone.md`
- Create: `tests/mic_standalone/send_test_tone.py`

**Interfaces:**
- Consumes: the running host from Tasks 1 to 8.
- Produces: a reference sender that the operator runs before Calliope exists, and the record of the manual verification.

The sender script pairs, starts a mic session, and sends Opus frames over UDP. It sends Opus-encoded silence frames, which verify pairing, the mic session, tray notices, the device switch, and the API. Audible audio is verified later with the `micsend` tool from the Calliope plan.

- [ ] **Step 1: Write the reference sender**

Create `tests/mic_standalone/send_test_tone.py`:

```python
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
```

Add `tests/mic_standalone/.send_test_tone.json` and `tests/mic_standalone/.send_test_tone.pem` to `.gitignore`.

- [ ] **Step 2: Write the user documentation**

Create `docs/remote_microphone.md`:

```markdown
# Remote microphone

Apollo can receive a microphone from an iPhone or a Mac running Calliope. The voice
arrives on the PC as the Steam Streaming Microphone, which Steam voice chat, games, and
Discord read as an ordinary microphone. Moonlight is not involved and needs no change.

## Requirements

- Apollo on Windows.
- Steam installed on the PC. Steam supplies the Steam Streaming Microphone driver.
- Calliope on an iPhone or a Mac on the same network as the PC.

## Pair a microphone

1. Open Calliope and choose the PC. Calliope shows a 4-digit PIN.
2. On the PC, open the Apollo web UI and go to the PIN page. The tray notice
   "Incoming microphone pairing request" opens it directly.
3. Choose the Microphone tab. Enter the PIN and, optionally, a name for the device.
4. Select Send. The device appears under Microphones at the bottom of the page.

A PIN is valid for 120 seconds. After three wrong PINs, Apollo cancels the request.
Tap Get a new PIN in Calliope to start again.

## Use the microphone

Tap Connect in Calliope. While connected:

- Apollo makes the Steam Streaming Microphone the default microphone on the PC.
- The tray shows "Microphone connected" with the device name.
- The Microphones list shows Connected beside the device.

When Calliope disconnects, Apollo restores the previous default microphone. Apollo also
restores it on exit, and at the next start after a crash.

One microphone is connected at a time. When a second paired device connects, it
replaces the first, and the first shows a message.

## Remove a microphone

On the PIN page, under Microphones, select the delete button beside the device. A
connected device disconnects immediately.

## Network

Calliope sends audio to UDP port 48002 when Apollo uses the default base port. The port
is the base port plus 13. Allow it through the Windows firewall for private networks.

## Home automation

`GET /api/mic/list` lists paired microphones and marks the connected one. The read-only
API key can read it. See [API](api.md).

## Troubleshooting

| Message | Action |
|---|---|
| Apollo cannot find the Steam Streaming Microphone | Install Steam on the PC, then connect again |
| Another program is blocking the Steam Streaming Microphone | Close Steam Remote Play or the program using the device, then connect again |
| This host cannot receive a microphone | The PC runs Apollo on Linux or macOS. The microphone needs Apollo on Windows |
```

- [ ] **Step 3: Windows check: build and unit tests**

On the Windows PC, the operator runs:

```bash
cmake -B cmake-build-mic -G Ninja -S . -DBUILD_TESTS=ON && ninja -C cmake-build-mic
./cmake-build-mic/tests/test_sunshine --gtest_filter='Mic*'
```

Expected: the build completes, and 45 tests PASS.

- [ ] **Step 4: Windows check: inert until paired**

The operator starts the new build with no `mic_state.json` present, then runs in PowerShell:

```powershell
Get-NetUDPEndpoint -LocalPort 48002 -ErrorAction SilentlyContinue
```

Expected: no output. The operator then starts a Moonlight stream and confirms that video, audio, and input behave as before.

- [ ] **Step 5: Windows check: pairing, session, and restore**

From the macOS machine, run:

```bash
pip3 install cryptography requests
python3 tests/mic_standalone/send_test_tone.py <PC address>
```

The operator confirms each item:

1. The tray shows "Incoming microphone pairing request", and a click opens the Microphone tab.
2. After the PIN is entered, the page shows the success text, and "Reference sender" appears under Microphones.
3. The script prints `pong error code: 0`.
4. The tray shows "Microphone connected: Reference sender".
5. Windows Sound settings show "Microphone (Steam Streaming Microphone)" as the default input.
6. `GET /api/mic/list` with the read-only API key returns the device with `"connected": true`.
7. A Moonlight stream started during the session is unaffected.
8. After the script ends, the tray shows "Microphone disconnected: Reference sender", and the previous default input is restored.
9. Restart Apollo. "Reference sender" is still listed under Microphones, and `mic_state.json` beside `sunshine_state.json` holds an empty `previous_default_capture`. This confirms that the store replaces its file correctly under the Windows toolchain.
10. In Windows Sound settings, open the properties of both "Speakers (Steam Streaming Microphone)" and "Microphone (Steam Streaming Microphone)". Both show 2 channels, 32 bit, 48000 Hz.

- [ ] **Step 6: Windows check: failure paths**

1. Run the script and end it with Ctrl+C after 5 seconds. Within 5 seconds, the tray shows "Microphone disconnected: Reference sender (connection lost)", and the default input is restored.
2. Run the script, then end the Apollo process in Task Manager during the session. Start Apollo again. The log contains "restoring the default capture device after an unclean exit", and the default input is restored.
3. Delete the device under Microphones. Run the script again. The script fails at the session request with HTTP 401. Delete `tests/mic_standalone/.send_test_tone.json` to pair again. Keep the `.pem` file, because the host certificate is unchanged.
4. Enter a wrong PIN three times on the Microphone tab. The script prints "Pairing expired".

- [ ] **Step 7: Commit**

```bash
git add docs/remote_microphone.md tests/mic_standalone/send_test_tone.py .gitignore
git commit -m "docs(mic): add remote microphone guide and reference sender"
```

---

## Out of scope for this plan

- The Calliope app, the `MicCore` package, and the `micsend` tool. They have their own plan, which copies `tests/fixtures/mic_vectors.json` from Task 1.
- Audible end-to-end verification. It needs a real Opus encoder, which arrives with `micsend`.
- Translations of the new strings. Crowdin manages every locale other than `en.json`.
