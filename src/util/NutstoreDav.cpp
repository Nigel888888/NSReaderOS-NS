#include "NutstoreDav.h"
#ifdef RICKYOS_PRODUCT

#include <base64.h>
#include <expat.h>
#include <Logging.h>

#include <cctype>
#include <cstdio>
#include <cstring>

#include "PluginHttp.h"

namespace nutstore {
namespace {

constexpr size_t MAX_PROPFIND_RESPONSE = 512 * 1024;
constexpr size_t MAX_ENTRIES = 300;

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string urlEncode(const std::string& s, bool keepSlash) {
  static const char* HEX = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '-' || c == '_' || c == '.' || c == '~' || (keepSlash && c == '/');
    if (plain) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(HEX[c >> 4]);
      out.push_back(HEX[c & 15]);
    }
  }
  return out;
}

bool urlDecode(const std::string& in, std::string& out) {
  out.clear();
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%') {
      if (i + 2 >= in.size()) return false;
      const int hi = hexVal(in[i + 1]), lo = hexVal(in[i + 2]);
      if (hi < 0 || lo < 0) return false;
      out.push_back(static_cast<char>(hi * 16 + lo));
      i += 2;
    } else {
      out.push_back(in[i]);
    }
  }
  return true;
}

bool supportedFile(const std::string& name) {
  const auto dot = name.rfind('.');
  if (dot == std::string::npos) return false;
  std::string ext = name.substr(dot + 1);
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext == "md" || ext == "txt" || ext == "epub";
}

// Extracts the direct child name from a DAV href, relative to `folder`.
// Handles absolute URLs and /dav.jianguoyun.com/... prefixed paths.
bool childNameFromHref(const std::string& href, const std::string& folder, std::string& name, bool& isDir) {
  (void)isDir;
  std::string path = href;
  const std::string origin = "https://dav.jianguoyun.com";
  if (path.rfind(origin, 0) == 0) path.erase(0, origin.size());
  // Strip query/fragment defensively.
  const auto q = path.find_first_of("?#");
  if (q != std::string::npos) path.erase(q);

  // `folder` is already the URL-encoded collection path (see runList).
  std::string marker = "/" + folder;
  const size_t pos = path.find(marker);
  if (pos == std::string::npos) return false;
  std::string rest = path.substr(pos + marker.size());
  while (!rest.empty() && rest.front() == '/') rest.erase(rest.begin());
  // Collection hrefs end with a trailing slash; drop it before the check.
  if (!rest.empty() && rest.back() == '/') rest.pop_back();
  if (rest.empty() || rest.find('/') != std::string::npos) return false;  // folder itself or nested
  if (!urlDecode(rest, name) || name.empty() || name == "." || name == "..") return false;
  for (unsigned char c : name)
    if (c < 32 || c == 127) return false;
  return true;
}

struct Parser {
  XML_Parser xml = nullptr;
  std::string folder;
  std::vector<Entry>* out = nullptr;
  int depth = 0;
  int responseDepth = 0;
  bool inHref = false;
  bool inSize = false;
  bool collection = false;
  std::string text;
  std::string href;
  uint64_t size = 0;
  bool failed = false;
  bool truncated = false;
};

const char* localName(const char* name) {
  const char* sep = std::strchr(name, '|');
  return sep ? sep + 1 : name;
}

void XMLCALL onStart(void* data, const char* name, const char** /*attrs*/) {
  auto* p = static_cast<Parser*>(data);
  if (++p->depth > 12) {
    p->failed = true;
    XML_StopParser(p->xml, XML_FALSE);
    return;
  }
  if (p->responseDepth) return;
  const char* tag = localName(name);
  if (!std::strcmp(tag, "response")) {
    p->responseDepth = p->depth;
    p->href.clear();
    p->collection = false;
    p->size = 0;
  }
}

void XMLCALL onText(void* data, const char* value, int len) {
  auto* p = static_cast<Parser*>(data);
  if ((p->inHref || p->inSize) && len > 0 && p->text.size() < 2048)
    p->text.append(value, static_cast<size_t>(len));
}

void XMLCALL onEnd(void* data, const char* name) {
  auto* p = static_cast<Parser*>(data);
  const char* tag = localName(name);
  if (p->inHref && !std::strcmp(tag, "href")) {
    size_t b = 0, e = p->text.size();
    while (b < e && std::isspace(static_cast<unsigned char>(p->text[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(p->text[e - 1]))) --e;
    p->href = p->text.substr(b, e - b);
    p->inHref = false;
  }
  if (p->inSize && !std::strcmp(tag, "getcontentlength")) {
    uint64_t value = 0;
    for (char c : p->text) {
      if (c < '0' || c > '9') continue;
      if (value > (UINT64_MAX - static_cast<uint64_t>(c - '0')) / 10) break;
      value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    p->size = value;
    p->inSize = false;
  }
  if (!std::strcmp(tag, "response") && p->responseDepth == p->depth) {
    std::string childName;
    if (!p->href.empty() && childNameFromHref(p->href, p->folder, childName, p->collection)) {
      Entry entry;
      entry.name = childName;
      entry.href = "/" + urlEncode(childName, false);
      entry.bytes = p->size;
      entry.isDirectory = p->collection;
      if (!entry.isDirectory && !supportedFile(childName)) {
        // skip unsupported files
      } else if (p->out->size() < MAX_ENTRIES) {
        p->out->push_back(std::move(entry));
      } else {
        p->truncated = true;
      }
    }
    p->responseDepth = 0;
  }
  if (p->depth) --p->depth;
}

// A start handler that also opens href text capture and detects <collection/>.
void XMLCALL onStartFull(void* data, const char* name, const char** attrs) {
  onStart(data, name, attrs);
  auto* p = static_cast<Parser*>(data);
  if (p->failed || !p->responseDepth) return;
  const char* tag = localName(name);
  if (!std::strcmp(tag, "href")) {
    p->inHref = true;
    p->text.clear();
  } else if (!std::strcmp(tag, "getcontentlength")) {
    p->inSize = true;
    p->text.clear();
  } else if (!std::strcmp(tag, "collection")) {
    p->collection = true;
  }
}

void XMLCALL doctype(void* data, const char*, const char*, const char*, int) {
  auto* p = static_cast<Parser*>(data);
  p->failed = true;
  XML_StopParser(p->xml, XML_FALSE);
}

Result parsePropfind(const std::string& xml, const std::string& folder, std::vector<Entry>& out) {
  Parser p;
  p.folder = folder;
  p.out = &out;
  p.xml = XML_ParserCreateNS("UTF-8", '|');
  if (!p.xml) return Result::ParseFailure;
  XML_SetUserData(p.xml, &p);
  XML_SetElementHandler(p.xml, onStartFull, onEnd);
  XML_SetCharacterDataHandler(p.xml, onText);
  XML_SetStartDoctypeDeclHandler(p.xml, doctype);
  XML_SetParamEntityParsing(p.xml, XML_PARAM_ENTITY_PARSING_NEVER);

  const bool ok = XML_Parse(p.xml, xml.data(), static_cast<int>(xml.size()), 1) == XML_STATUS_OK;
  const bool valid = ok && !p.failed;
  XML_ParserFree(p.xml);
  return valid ? Result::Ok : Result::ParseFailure;
}

Result runList(const std::string& folder, const std::string& account, const std::string& password,
               std::vector<Entry>* out, std::string& message, const std::string& subpath) {
  if (folder.empty() || account.empty() || password.empty()) {
    message = "missing credentials";
    return Result::AuthFailure;
  }
  std::string url = folderUrl(folder);
  // subpath is an already percent-encoded relative folder path with a trailing '/'.
  if (!subpath.empty()) url += subpath;
  const std::string creds = account + ":" + password;
  const std::string auth = std::string("Basic ") + base64::encode(creds.c_str()).c_str();
  pluginhttp::Headers headers;
  headers.emplace_back("Depth", "1");
  headers.emplace_back("Authorization", auth);
  String body;
  const int status = pluginhttp::request(nullptr, url, "PROPFIND", "", headers, body, MAX_PROPFIND_RESPONSE);
  if (status < 0) {
    message = "transport failure";
    return Result::TransportFailure;
  }
  if (status == 401 || status == 403) {
    message = "unauthorized";
    return Result::AuthFailure;
  }
  if (status != 207 && (status < 200 || status >= 300)) {
    message = "http " + std::to_string(status);
    return Result::TransportFailure;
  }
  // The href marker is the full collection path (folder + subpath), relative
  // to the DAV root. subpath's trailing '/' is folded into the folder prefix.
  std::string base = folder;
  while (!base.empty() && base.front() == '/') base.erase(base.begin());
  while (!base.empty() && base.back() == '/') base.pop_back();
  base = urlEncode(base, true);
  if (!subpath.empty()) {
    std::string sp = subpath;
    while (!sp.empty() && sp.back() == '/') sp.pop_back();
    if (!sp.empty()) base += "/" + sp;
  }
  std::vector<Entry> parsed;
  const Result r = parsePropfind(std::string(body.c_str(), body.length()), base, parsed);
  if (r != Result::Ok) {
    message = "xml parse failure";
    return r;
  }
  if (out) *out = std::move(parsed);
  message = "ok";
  return Result::Ok;
}

}  // namespace

std::string encodePath(const std::string& path) { return urlEncode(path, true); }

std::string folderUrl(const std::string& folder) {
  std::string f = folder;
  while (!f.empty() && f.front() == '/') f.erase(f.begin());
  while (!f.empty() && f.back() == '/') f.pop_back();
  return std::string(BASE_URL) + urlEncode(f, true) + "/";
}

std::string childUrl(const std::string& base, const std::string& childHref) {
  std::string c = childHref;
  while (!c.empty() && c.front() == '/') c.erase(c.begin());
  return base + c;
}

Result list(const std::string& folder, const std::string& account, const std::string& password,
            std::vector<Entry>& out, std::string& message, const std::string& subpath) {
  return runList(folder, account, password, &out, message, subpath);
}

Result verify(const std::string& folder, const std::string& account, const std::string& password,
              std::string& message) {
  return runList(folder, account, password, nullptr, message, "");
}

}  // namespace nutstore

#endif
