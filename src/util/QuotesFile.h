#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The quotes notebook: whole sentences the reader collected while looking up
// a word, grouped by the book they came from. Kept separate from the
// vocabulary deck -- a quote is a passage to revisit, a word is a card to
// review.
//
// One JSON index holds every quote (a few hundred at most, well under a
// megabyte). Each record carries the sentence, its source book, an optional
// page number, when it was saved and a star flag. The key is the FNV-1a hash
// of book + sentence so the same line can never be stored twice.
class QuotesFile {
 public:
  // Longest sentence kept (bytes). Longer ones are cut on a UTF-8 boundary.
  static constexpr size_t MAX_QUOTE_LENGTH = 1000;
  // Longest book title kept (bytes).
  static constexpr size_t MAX_BOOK_LENGTH = 160;

  enum class AddResult : uint8_t { Added, AlreadyPresent, Failed };

  struct Quote {
    std::string key;       // hex FNV-1a of book + sentence (stable id)
    std::string sentence;  // the highlighted line
    std::string book;      // source title, may be empty
    uint32_t timestamp = 0;
    uint16_t page = 0;  // 0 when unknown
    bool favorite = false;
  };

  // Reads every quote from disk. False when the file does not exist or is
  // unreadable; `quotes` is empty in that case.
  static bool load(std::vector<Quote>& quotes);

  // Appends one sentence. Returns AlreadyPresent if the same book + sentence
  // was saved before, Failed on any I/O error. `page` of 0 means unknown.
  static AddResult add(const std::string& sentence, const std::string& book, uint16_t page, uint32_t timestamp);

  // Sets/clears the star flag on one quote. False when it could not persist.
  static bool setFavorite(const std::string& key, bool favorite);

  // Removes one quote. False when the key was not present or I/O failed.
  static bool remove(const std::string& key);

  // Exports the whole notebook as a Kindle "My Clippings.txt" file at `path`
  // (normally /My Clippings.txt on the SD card root, easy to copy over USB).
  // Returns the number of highlights written, 0 when the notebook is empty,
  // or -1 on any I/O failure. Written atomically via a .tmp file.
  static int exportClippings(const std::string& path);

  // Stable id of a book + sentence pair (also used as the file key).
  static std::string makeKey(const std::string& sentence, const std::string& book);
};
