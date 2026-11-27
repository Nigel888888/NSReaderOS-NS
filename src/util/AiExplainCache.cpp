#include "AiExplainCache.h"

#ifdef RICKYOS_PRODUCT

#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>

constexpr char AiExplainCache::CACHE_DIR[];

std::string AiExplainCache::keyFor(const std::string& sentence) {
  MD5Builder md5;
  md5.begin();
  md5.add(sentence.c_str());
  md5.calculate();
  return std::string(md5.toString().c_str());
}

void AiExplainCache::ensureDir() {
  if (!Storage.exists(CACHE_DIR)) Storage.mkdir(CACHE_DIR);
}

bool AiExplainCache::has(const std::string& sentence) {
  if (sentence.empty()) return false;
  const std::string path = std::string(CACHE_DIR) + "/" + keyFor(sentence) + ".txt";
  return Storage.exists(path.c_str());
}

bool AiExplainCache::get(const std::string& sentence, std::string& out) {
  out.clear();
  if (sentence.empty()) return false;
  const std::string path = std::string(CACHE_DIR) + "/" + keyFor(sentence) + ".txt";
  if (!Storage.readFileToString("AIC", path.c_str(), MAX_EXPLANATION + 1, out)) return false;
  return !out.empty();
}

bool AiExplainCache::put(const std::string& sentence, const std::string& text) {
  if (sentence.empty() || text.empty()) return false;
  ensureDir();
  const std::string path = std::string(CACHE_DIR) + "/" + keyFor(sentence) + ".txt";
  // Bound what we persist: never write more than the client allows in a reply.
  String payload = text.size() > MAX_EXPLANATION ? String(text.substr(0, MAX_EXPLANATION).c_str()) : String(text.c_str());
  if (!Storage.writeFile(path.c_str(), payload)) {
    LOG_ERR("AIC", "cache write failed: %s", path.c_str());
    return false;
  }
  return true;
}

bool AiExplainCache::remove(const std::string& sentence) {
  if (sentence.empty()) return false;
  const std::string path = std::string(CACHE_DIR) + "/" + keyFor(sentence) + ".txt";
  return Storage.remove(path.c_str());
}

void AiExplainCache::clear() {
  if (!Storage.exists(CACHE_DIR)) return;
  auto files = Storage.listFiles(CACHE_DIR, 500);
  for (const auto& f : files) {
    const std::string path = std::string(CACHE_DIR) + "/" + f.c_str();
    Storage.remove(path.c_str());
  }
  LOG_INF("AIC", "cache cleared: %zu files", files.size());
}

#endif  // RICKYOS_PRODUCT
