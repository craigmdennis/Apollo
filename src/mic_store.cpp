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
    if (ec) {
      // Some Windows toolchains refuse to rename over an existing file.
      std::filesystem::remove(_file, ec);
      std::filesystem::rename(temp, _file, ec);
    }
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
