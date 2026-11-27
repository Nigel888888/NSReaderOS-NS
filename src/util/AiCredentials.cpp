#include "AiCredentials.h"

#ifdef RICKYOS_PRODUCT

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cstring>

AiCredentials::Config AiCredentials::cache_;
bool AiCredentials::cached_ = false;

constexpr char AiCredentials::DEFAULT_URL[];
constexpr char AiCredentials::DEFAULT_MODEL[];
constexpr char AiCredentials::CONFIG_PATH[];

namespace {
void ensureAiDir() {
  if (!Storage.exists("/.crosspoint/ai")) Storage.mkdir("/.crosspoint/ai");
}
}  // namespace

AiCredentials::Config AiCredentials::load() {
  if (cached_) return cache_;

  Config cfg;
  cfg.url = DEFAULT_URL;
  cfg.model = DEFAULT_MODEL;

  std::string raw;
  if (Storage.readFileToString("AIC", CONFIG_PATH, 4096, raw)) {
    JsonDocument doc;
    if (deserializeJson(doc, raw) == DeserializationError::Ok) {
      const char* url = doc["url"] | "";
      const char* model = doc["model"] | "";
      const char* keyEnc = doc["key_obf"] | "";
      if (url[0]) cfg.url = url;
      if (model[0]) cfg.model = model;
      if (keyEnc[0]) {
        bool ok = false;
        std::string key = obfuscation::deobfuscateFromBase64(keyEnc, MAX_KEY_LEN, &ok, nullptr);
        if (ok) cfg.key = std::move(key);
      }
    }
  }

  cache_ = std::move(cfg);
  cached_ = true;
  return cache_;
}

bool AiCredentials::save(const Config& config) {
  JsonDocument doc;
  doc["url"] = config.url.c_str();
  doc["model"] = config.model.c_str();
  // Obfuscate with the device MAC before writing; never store the raw key.
  doc["key_obf"] = obfuscation::obfuscateToBase64(config.key).c_str();

  String out;
  serializeJson(doc, out);
  ensureAiDir();
  if (!Storage.writeFile(CONFIG_PATH, out)) {
    LOG_ERR("AIC", "save failed: %s", CONFIG_PATH);
    return false;
  }
  cache_ = config;
  cached_ = true;
  return true;
}

bool AiCredentials::hasKey() {
  Config cfg = load();
  return !cfg.key.empty();
}

std::string AiCredentials::maskedKey() {
  Config cfg = load();
  if (cfg.key.empty()) return "—";
  if (cfg.key.size() <= 4) return "••••";
  return "••••" + cfg.key.substr(cfg.key.size() - 4);
}

void AiCredentials::invalidateCache() { cached_ = false; }

#endif  // RICKYOS_PRODUCT
