/**
 * @file src/steam_activity.h
 * @brief Reads the last Steam game, its art, and the desktop wallpaper for paired clients.
 *
 * The parsing, selection, and file lookup are platform-neutral and take explicit inputs, so
 * they build and test off Windows. Only locating Steam and the signed-in user is Windows-specific.
 */
#pragma once

// standard includes
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

namespace steam_activity {
  /**
   * @brief One node of a Steam KeyValues document. A leaf has a value; a section has children.
   */
  struct kv_t {
    std::string value;
    std::vector<std::pair<std::string, kv_t>> children;

    /**
     * @brief First direct child with this key, matched without regard to case.
     */
    const kv_t *find(std::string_view key) const;

    /**
     * @brief Follow a chain of keys from this node. Returns nullptr when any key is missing.
     */
    const kv_t *path(std::initializer_list<std::string_view> keys) const;
  };

  /**
   * @brief Parse Steam text KeyValues (quoted strings, braces, `//` comments).
   * @return A synthetic root whose children are the top-level keys.
   */
  kv_t parse_kv(std::string_view text);

  /// Largest KeyValues file that is read. A heavy `localconfig.vdf` is a few megabytes.
  constexpr uintmax_t max_kv_bytes = 32ULL << 20;

  /**
   * @brief Read and parse a KeyValues file. Nothing when it is missing, not a regular file, or over `max_kv_bytes`.
   */
  std::optional<kv_t> read_kv_file(const std::filesystem::path &file);

  constexpr uint64_t steam_id_base = 76561197960265728ULL;

  inline uint64_t to_steam_id(uint32_t account_id) {
    return steam_id_base + account_id;
  }

  /**
   * @brief Account ID of the `loginusers.vdf` entry that signed in last.
   *
   * Prefers `MostRecent` 1, then `AutoLogin` 1, then the newest `timestamp`.
   */
  std::optional<uint32_t> most_recent_account(const kv_t &loginusers);

  struct game_t {
    uint32_t app_id;
    std::string name;
    int64_t at;  ///< Unix seconds
  };

  struct activity_t {
    uint32_t account_id = 0;  ///< 0 when unknown
    std::optional<uint32_t> running_app_id;
    std::optional<std::string> running_name;
    std::vector<game_t> recent;  ///< Newest first, at most 5. The first entry is the last played game.
    int64_t updated_at = 0;
  };

  /**
   * @brief Build the activity for one account from the Steam files under `root`.
   * @param running_app_id 0 when nothing runs.
   */
  activity_t read_activity(const std::filesystem::path &root, uint32_t account_id, uint32_t running_app_id, int64_t now);

  nlohmann::json to_json(const activity_t &activity);

  /** @brief Response when the host switch or the client's permission withholds activity. */
  nlohmann::json json_withheld();

  /** @brief Response when Steam is not installed. */
  nlohmann::json json_no_steam();

  enum class art_kind {
    hero,
    logo,
    capsule,
  };

  std::optional<art_kind> parse_art_kind(std::string_view kind);

  /**
   * @brief Parse 1 to 10 decimal digits that fit in 32 bits.
   */
  std::optional<uint32_t> parse_app_id(std::string_view text);

  constexpr uintmax_t max_art_bytes = 8ULL << 20;
  constexpr uintmax_t max_wallpaper_bytes = 20ULL << 20;

  /** @brief An image held in memory, with its media type. */
  struct image_t {
    std::string type;
    std::string data;
  };

  /**
   * @brief Find the art file for an app in Steam's library cache. Handles the flat and nested layouts.
   * @return A file inside `<root>/appcache/librarycache` of at most `max_art_bytes`, or nothing.
   *         Nothing is returned when `appcache` or `librarycache` is a link, because a link could point outside Steam.
   */
  std::optional<std::filesystem::path> find_art(const std::filesystem::path &root, uint32_t app_id, art_kind kind);

  /**
   * @brief Media type from the file extension (jpg, png, webp). Empty for any other extension.
   */
  std::string_view media_type_for_extension(const std::filesystem::path &file);

  /**
   * @brief Media type from the first bytes: JPEG, PNG, or BMP. Empty for anything else.
   */
  std::string_view sniff_image_type(std::string_view head);

  /**
   * @brief Read an art file when its extension names a supported type and it fits in `max_art_bytes`.
   */
  std::optional<image_t> load_art(const std::filesystem::path &file);

  /**
   * @brief Read a wallpaper when its first bytes are a JPEG, PNG, or BMP and it fits in `max_wallpaper_bytes`.
   */
  std::optional<image_t> load_wallpaper(const std::filesystem::path &file);

  /**
   * @brief Choose the wallpaper file: the registry path when it names a file, else the transcoded copy.
   */
  std::optional<std::filesystem::path> pick_wallpaper(const std::filesystem::path &registry_path, const std::filesystem::path &transcoded);

  // Windows-backed lookups. On other platforms they report that nothing is installed.
  //
  // Apollo's process runs as SYSTEM. These functions read every file whose path a user can influence
  // (Steam's folders, the wallpaper) while impersonating the signed-in console user, so the user's own
  // access rights apply and a link cannot make Apollo read something the user cannot.

  /** @brief Steam root from the machine-wide registry key, or nothing when Steam is not installed. */
  std::optional<std::filesystem::path> locate_root();

  /**
   * @brief Activity for the signed-in user, cached for 30 seconds.
   * @return JSON for the full response, or `json_no_steam()`.
   */
  nlohmann::json current_activity();

  /** @brief Art for an app. Nothing when no user is signed in or no file matches. */
  std::optional<image_t> current_art(uint32_t app_id, art_kind kind);

  /** @brief Wallpaper of the signed-in console user. Nothing when no user is signed in. */
  std::optional<image_t> current_wallpaper();
}  // namespace steam_activity
