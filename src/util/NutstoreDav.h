#pragma once
#ifdef RICKYOS_PRODUCT

#include <cstdint>
#include <string>
#include <vector>

// Minimal WebDAV client for Nutstore (坚果云). All network work is bounded and
// runs synchronously on the caller's task (the CatalogActivity loading state
// keeps the device awake). Nothing here touches the UI or the settings store.
namespace nutstore {

constexpr const char* BASE_URL = "https://dav.jianguoyun.com/dav/";

struct Entry {
  std::string name;   // last path segment, decoded (file or folder name)
  std::string href;   // path relative to the sync folder, leading '/', for GET/child listing
  uint64_t bytes = 0;
  bool isDirectory = false;
};

// Percentage-encodes a relative path (slashes kept) for use in a URL.
std::string encodePath(const std::string& path);

// Builds "https://dav.jianguoyun.com/dav/<folder>/", percent-encoding the folder.
std::string folderUrl(const std::string& folder);

// Joins a folder URL and a child href (which starts with '/') into a full URL.
std::string childUrl(const std::string& folderUrl, const std::string& childHref);

// RESULT codes: Ok / TransportFailure / AuthFailure / ParseFailure / TooLarge.
enum class Result { Ok, TransportFailure, AuthFailure, ParseFailure, TooLarge, MissingFolder };

// Runs a bounded PROPFIND (Depth: 1) against the configured folder with HTTP
// Basic auth, parses the 207 Multi-Status XML, and returns direct children.
// Only md/txt/epub files and folders are returned; other files are skipped.
// `message` receives a short human-readable status on failure.
Result list(const std::string& folder, const std::string& account, const std::string& password,
            std::vector<Entry>& out, std::string& message, const std::string& subpath = "");

// Verifies credentials with a single PROPFIND; `out` may be null to discard entries.
Result verify(const std::string& folder, const std::string& account, const std::string& password,
              std::string& message);

// Ensures the configured sync folder exists at the DAV root: a PROPFIND that
// returns 404 is answered with MKCOL (WebDAV create-collection), then verified.
// Returns Ok when the folder is usable, otherwise the concrete failure. `message`
// receives a short status; `created` is set true when a folder was made.
Result ensureFolder(const std::string& folder, const std::string& account, const std::string& password,
                    std::string& message, bool* created = nullptr);

}  // namespace nutstore

#endif
