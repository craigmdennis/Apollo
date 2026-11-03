/**
 * @file src/steam_activity.cpp
 * @brief Platform-neutral parsing, selection, and file lookup for Steam activity.
 */
#include "steam_activity.h"

// standard includes
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <tuple>

namespace fs = std::filesystem;

namespace steam_activity {
  namespace {
    constexpr int max_depth = 64;

    bool iequals(std::string_view a, std::string_view b) {
      return std::ranges::equal(a, b, [](unsigned char x, unsigned char y) {
        return std::tolower(x) == std::tolower(y);
      });
    }

    enum class tok_e {
      text,
      open,
      close,
      end,
    };

    struct tok_t {
      tok_e kind;
      std::string text;
    };

    struct lexer_t {
      std::string_view s;
      size_t i = 0;

      tok_t next() {
        for (;;) {
          while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
            ++i;
          }
          if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') {
              ++i;
            }
            continue;
          }
          break;
        }
        if (i >= s.size()) {
          return {tok_e::end, {}};
        }
        char c = s[i];
        if (c == '{') {
          ++i;
          return {tok_e::open, {}};
        }
        if (c == '}') {
          ++i;
          return {tok_e::close, {}};
        }
        std::string out;
        if (c == '"') {
          ++i;
          while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
              ++i;
              out += s[i] == 'n' ? '\n' : s[i] == 't' ? '\t' : s[i];
            } else {
              out += s[i];
            }
            ++i;
          }
          if (i < s.size()) {
            ++i;
          }
          return {tok_e::text, std::move(out)};
        }
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '{' && s[i] != '}' && s[i] != '"') {
          out += s[i++];
        }
        return {tok_e::text, std::move(out)};
      }
    };

    // Reads entries until the closing brace or the end of input.
    void parse_block(lexer_t &lex, kv_t &into, int depth) {
      for (;;) {
        auto key = lex.next();
        if (key.kind != tok_e::text) {
          return;
        }
        auto val = lex.next();
        kv_t child;
        if (val.kind == tok_e::open) {
          if (depth >= max_depth) {
            return;
          }
          parse_block(lex, child, depth + 1);
        } else if (val.kind == tok_e::text) {
          child.value = std::move(val.text);
        } else {
          return;
        }
        into.children.emplace_back(std::move(key.text), std::move(child));
      }
    }

    // Reads at most max_bytes from the open file. The size is checked on the bytes read, not on a
    // separate stat, because the file can change between the two.
    std::optional<std::string> read_text(const fs::path &file, uintmax_t max_bytes) {
      std::ifstream in(file, std::ios::binary);
      if (!in) {
        return std::nullopt;
      }
      std::string text;
      char chunk[64 * 1024];
      while (in.read(chunk, sizeof(chunk)) || in.gcount() > 0) {
        text.append(chunk, static_cast<size_t>(in.gcount()));
        if (text.size() > max_bytes) {
          return std::nullopt;
        }
      }
      return text;
    }

    std::optional<kv_t> read_kv(const fs::path &file) {
      // A user who can write Steam's folder could otherwise make Apollo load a file of any size.
      std::error_code ec;
      if (!fs::is_regular_file(file, ec)) {
        return std::nullopt;
      }
      auto text = read_text(file, max_kv_bytes);
      if (!text) {
        return std::nullopt;
      }
      return parse_kv(*text);
    }

    std::optional<uint64_t> to_u64(std::string_view text) {
      if (text.empty() || text.size() > 20 || !std::ranges::all_of(text, [](unsigned char c) {
            return std::isdigit(c);
          })) {
        return std::nullopt;
      }
      return std::strtoull(std::string(text).c_str(), nullptr, 10);
    }

    // Windows reads both slashes as a separator, and two in a row at the start name a network share.
    bool is_separator(char c) {
      return c == '/' || c == '\\';
    }

    // Accepts only a local absolute path. On Windows that is a drive letter, a colon, and a separator,
    // which excludes every network, device and extended-length form (\\server, \\?\, \\.\, \??\).
    // Elsewhere it is a rooted path that does not start with two separators.
    bool is_local_path(const std::string &text) {
#ifdef _WIN32
      return text.size() >= 3 && std::isalpha(static_cast<unsigned char>(text[0])) && text[1] == ':' && is_separator(text[2]);
#else
      return text.size() > 1 && is_separator(text[0]) && !is_separator(text[1]);
#endif
    }

    // The main Steam folder first, then every library folder that libraryfolders.vdf lists.
    std::vector<fs::path> library_paths(const fs::path &root) {
      std::vector<fs::path> libs {root};
      auto doc = read_kv(root / "steamapps" / "libraryfolders.vdf");
      if (!doc) {
        return libs;
      }
      const kv_t *folders = doc->find("libraryfolders");
      if (!folders) {
        return libs;
      }
      for (const auto &[key, entry] : folders->children) {
        if (!to_u64(key)) {
          continue;
        }
        // Current format: an entry with a "path" child. Older format: the entry value is the path.
        const kv_t *path = entry.find("path");
        const std::string &text = path ? path->value : entry.value;
        if (text.empty()) {
          continue;
        }
        fs::path lib {text};
        // A relative path depends on the working directory, and a UNC path would make Apollo
        // authenticate to another machine. Neither is a Steam library on this PC.
        if (!is_local_path(text) || !lib.is_absolute()) {
          continue;
        }
        if (std::ranges::none_of(libs, [&](const fs::path &p) {
              return p.lexically_normal() == lib.lexically_normal();
            })) {
          libs.push_back(std::move(lib));
        }
      }
      return libs;
    }

    std::optional<std::string> app_name(const std::vector<fs::path> &libs, uint32_t app_id) {
      for (const auto &lib : libs) {
        auto doc = read_kv(lib / "steamapps" / ("appmanifest_" + std::to_string(app_id) + ".acf"));
        if (!doc) {
          continue;
        }
        const kv_t *name = doc->path({"AppState", "name"});
        if (name && !name->value.empty()) {
          return name->value;
        }
      }
      return std::nullopt;
    }

    bool inside(const fs::path &dir, const fs::path &file) {
      auto rel = file.lexically_relative(dir);
      return !rel.empty() && *rel.begin() != "..";
    }
  }  // namespace

  std::optional<kv_t> read_kv_file(const fs::path &file) {
    return read_kv(file);
  }

  const kv_t *kv_t::find(std::string_view key) const {
    for (const auto &[k, v] : children) {
      if (iequals(k, key)) {
        return &v;
      }
    }
    return nullptr;
  }

  const kv_t *kv_t::path(std::initializer_list<std::string_view> keys) const {
    const kv_t *node = this;
    for (auto key : keys) {
      node = node->find(key);
      if (!node) {
        return nullptr;
      }
    }
    return node;
  }

  kv_t parse_kv(std::string_view text) {
    kv_t root;
    lexer_t lex {text};
    parse_block(lex, root, 0);
    return root;
  }

  std::optional<uint32_t> most_recent_account(const kv_t &loginusers) {
    const kv_t *users = loginusers.find("users");
    if (!users) {
      return std::nullopt;
    }
    // Current Steam clients often omit MostRecent, so rank by MostRecent, then AutoLogin,
    // then the newest sign-in timestamp.
    auto flag = [](const kv_t &user, std::string_view name) {
      const kv_t *value = user.find(name);
      return value && value->value == "1";
    };
    std::optional<uint32_t> best;
    std::tuple<bool, bool, uint64_t> best_rank {};
    for (const auto &[key, user] : users->children) {
      auto id = to_u64(key);
      if (!id || *id <= steam_id_base || *id - steam_id_base > UINT32_MAX) {
        continue;
      }
      const kv_t *timestamp = user.find("timestamp");
      std::tuple<bool, bool, uint64_t> rank {
        flag(user, "MostRecent"),
        flag(user, "AutoLogin"),
        timestamp ? to_u64(timestamp->value).value_or(0) : 0
      };
      if (!best || rank > best_rank) {
        best = static_cast<uint32_t>(*id - steam_id_base);
        best_rank = rank;
      }
    }
    return best;
  }

  activity_t read_activity(const fs::path &root, uint32_t account_id, uint32_t running_app_id, int64_t now) {
    activity_t out;
    out.account_id = account_id;
    out.updated_at = now;

    auto libs = library_paths(root);

    if (running_app_id != 0) {
      out.running_app_id = running_app_id;
      out.running_name = app_name(libs, running_app_id);
    }

    if (account_id == 0) {
      return out;
    }
    auto config = read_kv(root / "userdata" / std::to_string(account_id) / "config" / "localconfig.vdf");
    const kv_t *apps = config ? config->path({"UserLocalConfigStore", "Software", "Valve", "Steam", "apps"}) : nullptr;
    if (!apps) {
      return out;
    }

    std::vector<std::pair<uint32_t, int64_t>> played;
    for (const auto &[key, app] : apps->children) {
      const kv_t *last = app.find("LastPlayed");
      auto id = to_u64(key);
      auto at = last ? to_u64(last->value) : std::nullopt;
      if (id && *id <= UINT32_MAX && at && *at > 0) {
        played.emplace_back(static_cast<uint32_t>(*id), static_cast<int64_t>(*at));
      }
    }
    std::ranges::sort(played, [](const auto &a, const auto &b) {
      return a.second != b.second ? a.second > b.second : a.first < b.first;
    });

    // An app with no manifest is uninstalled, and its name is not available locally.
    for (const auto &[id, at] : played) {
      if (out.recent.size() == 5) {
        break;
      }
      if (auto name = app_name(libs, id)) {
        out.recent.push_back({id, std::move(*name), at});
      }
    }
    return out;
  }

  nlohmann::json to_json(const activity_t &a) {
    auto game = [](const game_t &g) {
      return nlohmann::json {{"appId", g.app_id}, {"name", g.name}, {"at", g.at}};
    };
    nlohmann::json j;
    j["version"] = 1;
    j["steamInstalled"] = true;
    j["account"] = a.account_id ? nlohmann::json(std::to_string(to_steam_id(a.account_id))) : nlohmann::json(nullptr);
    j["runningAppId"] = a.running_app_id ? nlohmann::json(*a.running_app_id) : nlohmann::json(nullptr);
    j["runningName"] = a.running_name ? nlohmann::json(*a.running_name) : nlohmann::json(nullptr);
    j["lastPlayed"] = a.recent.empty() ? nlohmann::json(nullptr) : game(a.recent.front());
    j["recent"] = nlohmann::json::array();
    for (const auto &g : a.recent) {
      j["recent"].push_back(game(g));
    }
    j["updatedAt"] = a.updated_at;
    return j;
  }

  nlohmann::json json_withheld() {
    return {{"version", 1}, {"sharing", false}};
  }

  nlohmann::json json_no_steam() {
    return {{"version", 1}, {"steamInstalled", false}};
  }

  std::optional<art_kind> parse_art_kind(std::string_view kind) {
    if (kind == "hero") {
      return art_kind::hero;
    }
    if (kind == "logo") {
      return art_kind::logo;
    }
    if (kind == "capsule") {
      return art_kind::capsule;
    }
    return std::nullopt;
  }

  std::optional<uint32_t> parse_app_id(std::string_view text) {
    if (text.empty() || text.size() > 10) {
      return std::nullopt;
    }
    auto value = to_u64(text);
    if (!value || *value == 0 || *value > UINT32_MAX) {
      return std::nullopt;
    }
    return static_cast<uint32_t>(*value);
  }

  std::string_view media_type_for_extension(const fs::path &file) {
    std::string ext = file.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
      return std::tolower(c);
    });
    if (ext == ".jpg" || ext == ".jpeg") {
      return "image/jpeg";
    }
    if (ext == ".png") {
      return "image/png";
    }
    if (ext == ".webp") {
      return "image/webp";
    }
    return {};
  }

  std::string_view sniff_image_type(std::string_view head) {
    if (head.starts_with("\xFF\xD8\xFF")) {
      return "image/jpeg";
    }
    if (head.starts_with("\x89PNG")) {
      return "image/png";
    }
    if (head.starts_with("BM")) {
      return "image/bmp";
    }
    return {};
  }

  namespace {
    std::optional<std::string> read_limited(const fs::path &file, uintmax_t max_bytes) {
      std::error_code ec;
      if (!fs::is_regular_file(file, ec) || fs::file_size(file, ec) > max_bytes || ec) {
        return std::nullopt;
      }
      return read_text(file, max_bytes);
    }
  }  // namespace

  std::optional<image_t> load_art(const fs::path &file) {
    auto type = media_type_for_extension(file);
    auto data = type.empty() ? std::nullopt : read_limited(file, max_art_bytes);
    if (!data) {
      return std::nullopt;
    }
    return image_t {std::string(type), std::move(*data)};
  }

  std::optional<image_t> load_wallpaper(const fs::path &file) {
    auto data = read_limited(file, max_wallpaper_bytes);
    if (!data) {
      return std::nullopt;
    }
    auto type = sniff_image_type(*data);
    if (type.empty()) {
      return std::nullopt;
    }
    return image_t {std::string(type), std::move(*data)};
  }

  std::optional<fs::path> pick_wallpaper(const fs::path &registry_path, const fs::path &transcoded) {
    std::error_code ec;
    if (!registry_path.empty() && fs::is_regular_file(registry_path, ec)) {
      return registry_path;
    }
    if (!transcoded.empty() && fs::is_regular_file(transcoded, ec)) {
      return transcoded;
    }
    return std::nullopt;
  }

  std::optional<fs::path> find_art(const fs::path &root, uint32_t app_id, art_kind kind) {
    std::error_code ec;
    auto cache = fs::canonical(root / "appcache" / "librarycache", ec);
    auto expected = fs::canonical(root, ec) / "appcache" / "librarycache";
    // A link at appcache or librarycache makes the resolved path differ from the literal one.
    if (ec || !iequals(cache.generic_string(), expected.generic_string()) || !fs::is_directory(cache, ec)) {
      return std::nullopt;
    }

    // Names in preference order. The capsule falls back to the header image.
    std::vector<std::string> names;
    switch (kind) {
      case art_kind::hero:
        names = {"library_hero.jpg"};
        break;
      case art_kind::logo:
        names = {"logo.png"};
        break;
      case art_kind::capsule:
        // Current Steam clients write library_capsule.jpg and library_header.jpg instead.
        names = {"library_600x900.jpg", "library_capsule.jpg", "header.jpg", "library_header.jpg"};
        break;
    }

    auto accept = [&](const fs::path &candidate) -> std::optional<fs::path> {
      std::error_code e;
      if (!fs::is_regular_file(candidate, e) || fs::file_size(candidate, e) > max_art_bytes) {
        return std::nullopt;
      }
      auto canonical = fs::canonical(candidate, e);
      if (e || !inside(cache, canonical) || media_type_for_extension(canonical).empty()) {
        return std::nullopt;
      }
      return canonical;
    };

    const std::string id = std::to_string(app_id);
    for (const auto &name : names) {
      // Flat layout: librarycache/<appId>_<name>
      if (auto hit = accept(cache / (id + "_" + name))) {
        return hit;
      }
      // Nested layout: librarycache/<appId>/<hash>/<name>, hash folder name unknown.
      auto dir = cache / id;
      if (!fs::is_directory(dir, ec)) {
        continue;
      }
      if (auto hit = accept(dir / name)) {
        return hit;
      }
      std::set<fs::path> subdirs;
      for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_directory(ec)) {
          subdirs.insert(entry.path());
        }
      }
      for (const auto &sub : subdirs) {
        if (auto hit = accept(sub / name)) {
          return hit;
        }
      }
    }
    return std::nullopt;
  }
}  // namespace steam_activity
