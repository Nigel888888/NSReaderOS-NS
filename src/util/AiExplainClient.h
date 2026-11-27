#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace freeink {
class SecureHttpClient;
}

// Thin client for OpenAI-compatible chat-completions (DeepSeek). One-shot,
// non-streaming: request "explain this word in this sentence", get back a
// short answer string. Kept deliberately small — no conversation history,
// no SSE parsing — to fit the device's tight heap.
class AiExplainClient {
 public:
  struct Result {
    enum class Status { Ok, NoKey, NoWifi, HttpError, ParseError, Empty, Aborted };
    Status status = Status::Aborted;
    std::string text;    // assistant answer on Ok
    int httpStatus = 0;  // raw HTTP code on HttpError
    unsigned long elapsedMs = 0;

    static const char* label(Status s);
  };

  // Response body cap. DeepSeek's answer is bounded by max_tokens=512 on the
  // server; this only guards against a malformed/huge response.
  static constexpr size_t MAX_RESPONSE = 16 * 1024;

  static Result explain(const std::string& headword, const std::string& sentence, freeink::SecureHttpClient* session,
                        const std::function<bool()>& shouldAbort);
};
