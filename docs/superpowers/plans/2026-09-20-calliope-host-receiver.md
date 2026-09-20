# Calliope host receiver implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Apollo on Windows receives encrypted microphone audio from a paired mic device and plays it into the Steam Streaming Microphone, with no change to streaming.

**Architecture:** Four platform-neutral units (packet protocol, jitter buffer, mic device store, pairing state) are built and tested first, on any machine. A Windows writer plays PCM into the virtual microphone. An orchestrator in `src/mic.cpp` owns one thread, one UDP socket, and one mic session. Seven config server routes, four tray functions, and a Microphone tab on the PIN page connect the orchestrator to the outside.

**Tech stack:** C++23, OpenSSL (AES-128-GCM, HMAC-SHA256), Boost.Asio UDP, libopus, nlohmann/json, WASAPI, GoogleTest, Vue 3.

**Spec:** `docs/superpowers/specs/2026-09-20-calliope-mic-sidecar-design.md`. This plan covers build-order items 1, 2, and the host half of item 5. The Calliope app has its own plan.

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
- Jitter buffer: 40 ms prebuffer (2 frames), 200 ms maximum (10 frames).
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
| `src/mic.h`, `.cpp` | Orchestrator: thread, UDP socket, mic session, Opus decode, tray calls |
| `src/platform/windows/mic_write.cpp` | WASAPI writer for the virtual microphone, driver install, default capture device |
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

### Task 5: Platform interface and the Windows virtual microphone writer

**Files:**
- Modify: `src/platform/common.h` (after `class mic_t`, near line 553)
- Create: `src/platform/windows/mic_write.cpp`
- Modify: `src/platform/linux/audio.cpp` (end of `namespace platf`)
- Modify: `src/platform/macos/microphone.mm` (end of `namespace platf`)
- Modify: `cmake/compile_definitions/windows.cmake:61`

**Interfaces:**
- Consumes: `util::safe_ptr`, `platf::from_utf8`, `platf::to_utf8` from `src/platform/windows/misc.h`, `IPolicyConfig` from `src/platform/windows/PolicyConfig.h`.
- Produces, in namespace `platf`:
  - `class virtual_mic_t` with `virtual bool write(const float *mono_samples, std::size_t count) = 0` and `virtual std::string previous_default_capture() const = 0`
  - `enum class virtual_mic_error_e { none, unsupported, device_missing, device_open_failed }`
  - `std::unique_ptr<virtual_mic_t> virtual_mic(virtual_mic_error_e &error)`. The caller's thread owns the returned object. Destruction restores the previous default capture device.
  - `void restore_default_capture(const std::string &device_id)`

This task has no unit test. WASAPI and the Steam driver exist only on the Windows PC, so the check is a Windows build followed by the manual verification in Task 9.

The device lookup and the event-driven render loop are adapted from Apollo pull request #1428, which is GPL-3.0 like Apollo. The adaptation drops that pull request's Opus decode and packet queue, because `src/mic.cpp` owns those.

- [ ] **Step 1: Add the interface to `src/platform/common.h`**

After the closing brace of `class mic_t` and before `class audio_control_t`, add:

```cpp
  /**
   * @brief A host capture device that plays PCM supplied by Apollo.
   *
   * On Windows this is the Steam Streaming Microphone. Creating one makes it the default
   * capture device. Destroying it restores the previous default capture device.
   */
  class virtual_mic_t {
  public:
    /**
     * @brief Queue 48 kHz mono float samples for playout.
     * @return false when the device stopped accepting audio.
     */
    virtual bool write(const float *mono_samples, std::size_t count) = 0;

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
   * @brief Open the virtual microphone on the calling thread.
   * @param error Receives the reason when the result is null.
   */
  std::unique_ptr<virtual_mic_t> virtual_mic(virtual_mic_error_e &error);

  /**
   * @brief Make the given device the default capture device. Used after a crash during a mic session.
   */
  void restore_default_capture(const std::string &device_id);
```

- [ ] **Step 2: Add the factories for Linux and macOS**

At the end of `namespace platf` in `src/platform/linux/audio.cpp`, add:

```cpp
  std::unique_ptr<virtual_mic_t> virtual_mic(virtual_mic_error_e &error) {
    error = virtual_mic_error_e::unsupported;
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
 * @brief Plays PCM into the Steam Streaming Microphone and manages the default capture device.
 *
 * Device lookup and the event-driven render loop are adapted from Apollo pull request #1428
 * (logabell/apollo-microphone), licensed GPL-3.0.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

// platform includes
#include <Audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <newdev.h>
#include <synchapi.h>

// local includes
#include "misc.h"
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
    constexpr REFERENCE_TIME BUFFER_DURATION_100NS = 1000000;  // 100 ms
    constexpr std::size_t MAX_QUEUED_SAMPLES = SAMPLE_RATE / 5;  // 200 ms
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
          auto name = lower(prop_string(props.get(), key));
          // Steam also ships a 16-channel variant, which voice chat applications cannot use.
          if (name.find(DEVICE_NAME_PATTERN) != std::wstring::npos && name.find(L"16ch") == std::wstring::npos) {
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
        BOOST_LOG(warning) << "Remote microphone: could not install the Steam Streaming Microphone driver: "sv << GetLastError();
        return false;
      }

      BOOST_LOG(info) << "Remote microphone: installed the Steam Streaming Microphone driver"sv;
      // The audio subsystem needs time to publish the new endpoints.
      Sleep(5000);
      return true;
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

    class wasapi_virtual_mic_t: public virtual_mic_t {
    public:
      ~wasapi_virtual_mic_t() override {
        stop = true;
        if (render_thread.joinable()) {
          render_thread.join();
        }
        if (audio_client) {
          audio_client->Stop();
        }
        if (!previous_default.empty()) {
          set_default_capture(previous_default);
        }
        render_client.reset();
        audio_client.reset();
        if (render_event) {
          CloseHandle(render_event);
        }
        if (com_initialized) {
          CoUninitialize();
        }
      }

      bool init(virtual_mic_error_e &error) {
        com_initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY));

        auto device_enum = create_enumerator();
        if (!device_enum) {
          error = virtual_mic_error_e::device_open_failed;
          return false;
        }

        auto render_id = find_steam_endpoint(device_enum.get(), eRender);
        if (!render_id && install_steam_mic_driver()) {
          render_id = find_steam_endpoint(device_enum.get(), eRender);
        }
        auto capture_id = find_steam_endpoint(device_enum.get(), eCapture);
        if (!render_id || !capture_id) {
          BOOST_LOG(warning) << "Remote microphone: the Steam Streaming Microphone was not found. Install Steam on this PC."sv;
          error = virtual_mic_error_e::device_missing;
          return false;
        }

        device_t device;
        if (FAILED(device_enum->GetDevice(render_id->c_str(), &device)) || !device ||
            FAILED(device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void **) &audio_client)) || !audio_client) {
          error = virtual_mic_error_e::device_open_failed;
          return false;
        }

        WAVEFORMATEXTENSIBLE format {};
        format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        format.Format.nChannels = 2;
        format.Format.nSamplesPerSec = SAMPLE_RATE;
        format.Format.wBitsPerSample = 32;
        format.Format.nBlockAlign = static_cast<WORD>(format.Format.nChannels * sizeof(float));
        format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
        format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        format.Samples.wValidBitsPerSample = 32;
        format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

        // ponytail: AUTOCONVERTPCM lets Windows convert to the device format. If a capture application
        // reads distorted audio, port ensure_recommended_steam_mic_format() from pull request #1428,
        // which forces both endpoints to 2 channels, 32-bit, 48000 Hz.
        auto status = audio_client->Initialize(
          AUDCLNT_SHAREMODE_SHARED,
          AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
          BUFFER_DURATION_100NS,
          0,
          &format.Format,
          nullptr
        );
        if (FAILED(status)) {
          BOOST_LOG(error) << "Remote microphone: could not initialize the Steam Streaming Microphone: 0x"sv << util::hex(status).to_string_view();
          error = virtual_mic_error_e::device_open_failed;
          return false;
        }

        render_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!render_event ||
            FAILED(audio_client->GetBufferSize(&buffer_frames)) ||
            FAILED(audio_client->GetService(IID_IAudioRenderClient, (void **) &render_client)) || !render_client ||
            FAILED(audio_client->SetEventHandle(render_event)) ||
            FAILED(audio_client->Start())) {
          error = virtual_mic_error_e::device_open_failed;
          return false;
        }

        previous_default = default_capture_id(device_enum.get());
        if (previous_default == *capture_id) {
          // A stale switch is already in place. Keeping it as "previous" would make the restore a no-op forever.
          previous_default.clear();
        }
        set_default_capture(*capture_id);

        render_thread = std::thread {[this]() {
          render_loop();
        }};
        return true;
      }

      bool write(const float *mono_samples, std::size_t count) override {
        if (failed) {
          return false;
        }
        std::lock_guard lock {queue_mutex};
        queue.insert(queue.end(), mono_samples, mono_samples + count);
        while (queue.size() > MAX_QUEUED_SAMPLES) {
          queue.pop_front();
        }
        return true;
      }

      std::string previous_default_capture() const override {
        return to_utf8(previous_default);
      }

    private:
      void render_loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY);
        platf::adjust_thread_priority(platf::thread_priority_e::high);

        while (!stop) {
          WaitForSingleObject(render_event, 20);
          if (stop) {
            break;
          }

          UINT32 padding = 0;
          if (FAILED(audio_client->GetCurrentPadding(&padding))) {
            failed = true;
            break;
          }

          UINT32 frames = buffer_frames - padding;
          {
            std::lock_guard lock {queue_mutex};
            frames = std::min<UINT32>(frames, static_cast<UINT32>(queue.size()));
          }
          if (frames == 0) {
            continue;
          }

          BYTE *buffer = nullptr;
          if (FAILED(render_client->GetBuffer(frames, &buffer)) || !buffer) {
            failed = true;
            break;
          }

          auto *out = reinterpret_cast<float *>(buffer);
          {
            std::lock_guard lock {queue_mutex};
            for (UINT32 frame = 0; frame < frames; ++frame) {
              auto sample = queue.front();
              queue.pop_front();
              out[frame * 2] = sample;
              out[frame * 2 + 1] = sample;
            }
          }
          render_client->ReleaseBuffer(frames, 0);
        }

        CoUninitialize();
      }

      audio_client_t audio_client;
      render_client_t render_client;
      HANDLE render_event = nullptr;
      UINT32 buffer_frames = 0;
      std::wstring previous_default;
      bool com_initialized = false;

      std::mutex queue_mutex;
      std::deque<float> queue;
      std::thread render_thread;
      std::atomic<bool> stop {false};
      std::atomic<bool> failed {false};
    };
  }  // namespace

  std::unique_ptr<virtual_mic_t> virtual_mic(virtual_mic_error_e &error) {
    error = virtual_mic_error_e::none;
    auto mic = std::make_unique<wasapi_virtual_mic_t>();
    if (!mic->init(error)) {
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

Expected: the build completes. `mic_write.cpp` compiles with no error. When `CLSID_MMDeviceEnumerator` or `KSDATAFORMAT_SUBTYPE_IEEE_FLOAT` is reported as undefined at link time, add `#define INITGUID` as the first line of `mic_write.cpp` and rebuild.

Also confirm the driver file exists:

```bash
ls "/c/Program Files (x86)/Common Files/Steam/drivers/Windows10/x64/" | grep -i microphone
```

Expected: `SteamStreamingMicrophone.inf`. When the file has a different name, update `STEAM_MIC_DRIVER_PATH` to match.

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

Threading model: the mic thread runs one `io_context`. The mic session, the socket, the decoder, and the virtual microphone are touched only on that thread. Config server handlers reach them by posting a task and waiting on a future. One mutex guards the store, the pairing state, and the connected device id, which handlers read directly.

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
    constexpr int FRAME_SAMPLES = 960;  // 20 ms at 48 kHz
    constexpr auto TICK = 20ms;
    constexpr auto SESSION_TIMEOUT = 5s;
    constexpr auto REPLACED_LIFETIME = 5s;
    constexpr int MAX_DECODE_FAILURES = 25;  // 500 ms of consecutive failures

    struct session_t {
      std::uint32_t id;
      crypto::cipher::gcm_t cipher;
      device_t device;
      steady::time_point last_valid;
      jitter_buffer_t jitter;
      std::unique_ptr<platf::virtual_mic_t> vmic;
      OpusDecoder *decoder = nullptr;
      protocol::error_e error = protocol::error_e::none;
      int decode_failures = 0;
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

    void notify_error(protocol::error_e error) {
      std::string text;
      switch (error) {
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
     * @brief End the mic session on the mic thread. Destroying the virtual microphone restores the default capture device.
     */
    void end_session(const std::string &reason, bool notify) {
      if (!session) {
        return;
      }

      auto name = session->device.name;
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

      platf::virtual_mic_error_e vmic_error = platf::virtual_mic_error_e::none;
      auto vmic = platf::virtual_mic(vmic_error);
      if (vmic_error == platf::virtual_mic_error_e::unsupported) {
        return session_error_e::unsupported;
      }

      auto key_bytes = crypto::rand(16);
      crypto::aes_t key {key_bytes.begin(), key_bytes.end()};
      std::uint32_t id = 0;
      auto id_bytes = crypto::rand(4);
      std::memcpy(&id, id_bytes.data(), sizeof(id));

      session = std::make_unique<session_t>();
      session->id = id;
      session->cipher = crypto::cipher::gcm_t {key, false};
      session->device = device;
      session->last_valid = steady::now();
      session->vmic = std::move(vmic);

      if (vmic_error == platf::virtual_mic_error_e::device_missing) {
        session->error = protocol::error_e::device_missing;
      } else if (vmic_error == platf::virtual_mic_error_e::device_open_failed) {
        session->error = protocol::error_e::device_open_failed;
      }

      int opus_error = OPUS_OK;
      session->decoder = opus_decoder_create(48000, 1, &opus_error);
      if (opus_error != OPUS_OK) {
        session->decoder = nullptr;
        session->error = protocol::error_e::decode_failed;
      }

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

    void send_pong(crypto::cipher::gcm_t &cipher, std::uint32_t session_id, std::uint32_t sequence, protocol::error_e error) {
      const char code = static_cast<char>(error);
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

    void play_one_frame() {
      if (!session->vmic || !session->decoder) {
        return;
      }

      auto next = session->jitter.pop();
      if (next.kind == jitter_buffer_t::pop_e::wait) {
        return;
      }

      std::array<float, FRAME_SAMPLES> pcm;
      int samples;
      if (next.kind == jitter_buffer_t::pop_e::frame) {
        samples = opus_decode_float(session->decoder, next.payload.data(), static_cast<opus_int32>(next.payload.size()), pcm.data(), FRAME_SAMPLES, 0);
      } else {
        // A null payload asks Opus to conceal one lost frame.
        samples = opus_decode_float(session->decoder, nullptr, 0, pcm.data(), FRAME_SAMPLES, 0);
      }

      if (samples < 0) {
        if (++session->decode_failures == MAX_DECODE_FAILURES) {
          session->error = protocol::error_e::decode_failed;
          notify_error(session->error);
        }
        return;
      }

      session->decode_failures = 0;
      if (!session->vmic->write(pcm.data(), static_cast<std::size_t>(samples))) {
        session->error = protocol::error_e::device_open_failed;
        notify_error(session->error);
        session->vmic.reset();
      }
    }

    void on_tick(const boost::system::error_code &ec) {
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
          play_one_frame();
        }
      }

      // Schedule from the previous expiry so the 20 ms period does not drift.
      timer->expires_at(timer->expiry() + TICK);
      timer->async_wait(on_tick);
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
        timer->expires_after(TICK);
        timer->async_wait(on_tick);
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

Expected: three exit codes of 0, then 36 tests PASS. When the syntax check reports that `<opus/opus.h>` is missing, run `brew install opus` and repeat.

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

Expected: the build completes, and 36 tests PASS.

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
