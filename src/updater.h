/**
 * @file src/updater.h
 * @brief Declarations for updating Apollo from a published release.
 */
#pragma once

/**
 * @brief Handles checking for and applying Apollo updates.
 */
namespace updater {
  /**
   * @brief Check GitHub for a newer Apollo release and update in place if the user agrees.
   * @details The check is performed by a helper process detached from Apollo, because
   *          installing an update stops the Apollo service and that terminates Apollo itself.
   *          Pairings and configuration are left alone; only the program files are replaced.
   * @return `true` if the helper process was started, `false` otherwise.
   * @examples
   * updater::check_for_updates();
   * @examples_end
   */
  bool check_for_updates();
}  // namespace updater
