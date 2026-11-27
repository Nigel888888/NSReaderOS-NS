#pragma once

#include <cstddef>
#include <string>

// Stores the user-entered DeepSeek-compatible endpoint, model name and API key
// on the SD card. The key is obfuscated with the device MAC (same scheme as the
// KOReader sync password) so it is not plain-text on the card, and is never
// written to logs, source, or OTA dumps. UI must render only a masked suffix.
class AiCredentials {
 public:
  struct Config {
    std::string url;    // full chat-completions URL
    std::string model;  // e.g. "deepseek-chat"
    std::string key;    // API key (obfuscated at rest)
  };

  static constexpr char DEFAULT_URL[] = "https://api.deepseek.com/v1/chat/completions";
  static constexpr char DEFAULT_MODEL[] = "deepseek-chat";
  static constexpr size_t MAX_KEY_LEN = 256;
  static constexpr size_t MAX_URL_LEN = 256;
  static constexpr size_t MAX_MODEL_LEN = 64;

  // Loads config from SD. On first run returns the built-in DeepSeek defaults
  // with an empty key. Safe to call repeatedly; result is cached in RAM.
  static Config load();

  // Persists config to SD (creating the directory if needed). The key is
  // obfuscated on disk. Returns false on SD write failure.
  static bool save(const Config& config);

  // True when a non-empty key is configured.
  static bool hasKey();

  // Returns "••••" + last 4 chars of the key, or "—" when unset. Used by the
  // settings UI; never returns the full key.
  static std::string maskedKey();

  // Clears the cached copy ( forces a reload next time ).
  static void invalidateCache();

 private:
  static constexpr char CONFIG_PATH[] = "/.crosspoint/ai/ai_config.json";
  static Config cache_;
  static bool cached_;
};
