#include "AiExplainClient.h"

#ifdef RICKYOS_PRODUCT

#include <ArduinoJson.h>
#include <SecureHttpClient.h>
#include <WiFi.h>

#include <cstdio>

#include "AiCredentials.h"
#include "util/PluginHttp.h"

const char* AiExplainClient::Result::label(Status s) {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::NoKey: return "no-key";
    case Status::NoWifi: return "no-wifi";
    case Status::HttpError: return "http-error";
    case Status::ParseError: return "parse-error";
    case Status::Empty: return "empty";
    case Status::Aborted: return "aborted";
  }
  return "?";
}

namespace {

// Shared POST/parse for both prompt flavours. Kept file-local so the two
// public entry points only differ in how they phrase the user message.
AiExplainClient::Result postPrompt(const std::string& userText, freeink::SecureHttpClient* session,
                                   const std::function<bool()>& shouldAbort) {
  using Status = AiExplainClient::Result::Status;
  AiExplainClient::Result result;
  const unsigned long started = millis();

  AiCredentials::Config cfg = AiCredentials::load();
  if (cfg.key.empty()) {
    result.status = Status::NoKey;
    return result;
  }
  if (WiFi.status() != WL_CONNECTED) {
    result.status = Status::NoWifi;
    return result;
  }

  JsonDocument req;
  req["model"] = cfg.model.c_str();
  req["stream"] = false;
  // Three labelled sections (paraphrase / translation / language points) can
  // run longer than the old one-liner; 700 leaves room for the last section
  // without inflating the token bill much.
  req["max_tokens"] = 700;
  JsonArray msgs = req["messages"].to<JsonArray>();
  {
    JsonObject sys = msgs.add<JsonObject>();
    sys["role"] = "system";
    // One shared answer shape for both entry points (word lookup and whole
    // sentence). Plain text with literal labels so the renderer shows it as-is.
    sys["content"] =
        "You are a concise English reading assistant. Always answer in exactly three sections, "
        "in this order, each beginning on its own line with the label shown, followed by a colon:\n"
        "Paraphrase: <restate the sentence's meaning in simple, clear English, in context>\n"
        "Translation: <give an accurate Chinese translation that fits the context>\n"
        "Language points: <briefly explain only the genuinely difficult items in the sentence - "
        "tricky phrases, fixed collocations, slang, idioms or proverbs. If there are none, say none. "
        "Do not explain ordinary words or grammar>\n"
        "Keep each section brief. Use plain text only, no markdown, no extra headings.";
  }
  {
    JsonObject usr = msgs.add<JsonObject>();
    usr["role"] = "user";
    usr["content"] = userText.c_str();
  }
  std::string body;  // request() takes const std::string& (Arduino String has no such conversion)
  serializeJson(req, body);

  pluginhttp::Headers headers;
  headers.emplace_back("Authorization", "Bearer " + cfg.key);
  headers.emplace_back("Content-Type", "application/json");

  String response;
  const int status = pluginhttp::request(session, cfg.url, "POST", body, headers, response, AiExplainClient::MAX_RESPONSE,
                                         nullptr, shouldAbort);
  result.elapsedMs = millis() - started;
  result.httpStatus = status;

  if (status < 200 || status >= 300) {
    result.status = Status::HttpError;
    return result;
  }

  JsonDocument doc;
  if (deserializeJson(doc, response) != DeserializationError::Ok) {
    result.status = Status::ParseError;
    return result;
  }
  const char* content = doc["choices"][0]["message"]["content"] | "";
  if (!content[0]) {
    result.status = Status::Empty;
    return result;
  }
  result.text = content;
  result.status = Status::Ok;
  return result;
}

}  // namespace

AiExplainClient::Result AiExplainClient::explain(const std::string& headword, const std::string& sentence,
                                                 freeink::SecureHttpClient* session,
                                                 const std::function<bool()>& shouldAbort) {
  // "word + sentence" context: the three-section answer is about this sentence,
  // with the highlighted word called out so language points can touch it.
  std::string userText;
  if (!sentence.empty()) {
    std::string ctx = sentence;
    if (ctx.size() > 900) ctx.resize(900);  // bound prompt size -> token cost
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "Explain the following English sentence. The reader selected "
                  "the word/phrase \"%s\" in it (you may mention it under Language points if relevant):\n%s",
                  headword.c_str(), ctx.c_str());
    userText = buf;
  } else {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "Explain the word/phrase \"%s\" using the three sections.", headword.c_str());
    userText = buf;
  }
  return postPrompt(userText, session, shouldAbort);
}

AiExplainClient::Result AiExplainClient::explainSentence(const std::string& sentence,
                                                         freeink::SecureHttpClient* session,
                                                         const std::function<bool()>& shouldAbort) {
  // No headword: a good-sentence collected from RSS is the whole input. Ask for
  // the three sections explicitly (the system prompt already defines them).
  std::string ctx = sentence;
  if (ctx.size() > 900) ctx.resize(900);
  char buf[1024];
  std::snprintf(buf, sizeof(buf), "Explain this English sentence with the three sections:\n%s", ctx.c_str());
  return postPrompt(buf, session, shouldAbort);
}

#endif  // RICKYOS_PRODUCT
