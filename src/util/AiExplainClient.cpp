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

AiExplainClient::Result AiExplainClient::explain(const std::string& headword, const std::string& sentence,
                                                 freeink::SecureHttpClient* session,
                                                 const std::function<bool()>& shouldAbort) {
  Result result;
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

  // Build the request. "word + sentence" context: the model explains the
  // marked word as used in that line, far more accurate than an isolated word.
  std::string userText;
  if (!sentence.empty()) {
    std::string ctx = sentence;
    if (ctx.size() > 900) ctx.resize(900);  // bound prompt size -> token cost
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "Explain the word/phrase \"%s\" as used in this sentence. "
                  "Reply concisely in English, then give a short Chinese translation:\n%s",
                  headword.c_str(), ctx.c_str());
    userText = buf;
  } else {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "Explain the word/phrase \"%s\" concisely in English, "
                  "then give a short Chinese translation.", headword.c_str());
    userText = buf;
  }

  JsonDocument req;
  req["model"] = cfg.model.c_str();
  req["stream"] = false;
  req["max_tokens"] = 512;
  JsonArray msgs = req["messages"].to<JsonArray>();
  {
    JsonObject sys = msgs.add<JsonObject>();
    sys["role"] = "system";
    sys["content"] = "You are a concise English reading assistant. Be brief and accurate.";
  }
  {
    JsonObject usr = msgs.add<JsonObject>();
    usr["role"] = "user";
    usr["content"] = userText.c_str();
  }
  String body;
  serializeJson(req, body);

  pluginhttp::Headers headers;
  headers.emplace_back("Authorization", "Bearer " + cfg.key);
  headers.emplace_back("Content-Type", "application/json");

  String response;
  const int status = pluginhttp::request(session, cfg.url, "POST", body, headers, response, MAX_RESPONSE,
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

#endif  // RICKYOS_PRODUCT
