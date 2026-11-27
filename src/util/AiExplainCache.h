#pragma once

#include <cstddef>
#include <string>

// Persistent cache of DeepSeek explanations, keyed by the sentence they
// explain. A quote saved from the dictionary card keeps its explanation in
// this cache, so the Quotes notebook can show it offline with no API call and
// no token cost. One small text file per sentence; answers are bounded by
// AiExplainClient::MAX_RESPONSE when written.
class AiExplainCache {
 public:
  // Longest explanation persisted (bytes). Matches the client response cap.
  static constexpr size_t MAX_EXPLANATION = 16 * 1024;

  // True when a cached explanation exists for this sentence.
  static bool has(const std::string& sentence);

  // Reads the cached explanation into `out`. Returns false on miss/IO error.
  static bool get(const std::string& sentence, std::string& out);

  // Persists an explanation for this sentence. Returns false on IO error.
  static bool put(const std::string& sentence, const std::string& text);

  // Deletes one cached explanation.
  static bool remove(const std::string& sentence);

  // Removes every cached explanation (used by "clear cache" in settings).
  static void clear();

  // Stable file name key: lower-case hex MD5 of the sentence.
  static std::string keyFor(const std::string& sentence);

 private:
  static constexpr char CACHE_DIR[] = "/.crosspoint/ai/cache";
  static void ensureDir();
};
