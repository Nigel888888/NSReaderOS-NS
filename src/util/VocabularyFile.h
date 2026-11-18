#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "util/study/StudyScheduler.h"

// The vocabulary notebook: words collected from the dictionary card, each one
// scheduled for review with FSRS-5 (the engine Anki runs).
//
// Two files back it, because the two halves have very different shapes:
//
//   /.crosspoint/vocabulary.json          the index: headword, a one-line
//                                         excerpt, the source book and the
//                                         card's scheduling state. Small
//                                         enough to hold every word in RAM.
//   /.crosspoint/vocabulary-defs/<key>.txt the long text of one word: the
//                                         sentence it was collected from and
//                                         its full definition. Read one at a
//                                         time, when a card is turned over.
//
// The index holds a summary rather than the full definition, so a notebook of
// several hundred words costs tens of kilobytes instead of hundreds.
class VocabularyFile {
 public:
  // Longest headword kept (bytes). Longer heads are truncated.
  static constexpr size_t MAX_HEADWORD_LENGTH = 96;
  // Longest excerpt kept in the index for a list row (bytes).
  static constexpr size_t MAX_SUMMARY_LENGTH = 160;
  // Longest definition kept on disk (bytes).
  static constexpr size_t MAX_DEFINITION_LENGTH = 2048;
  // Longest sentence kept on disk (bytes).
  static constexpr size_t MAX_SENTENCE_LENGTH = 400;

  enum class AddResult : uint8_t { Added, AlreadyPresent, Failed };

  // One saved word as the list screen holds it.
  struct Entry {
    std::string headword;
    std::string summary;  // one-line excerpt of the definition
    std::string book;     // the book it was collected from, may be empty
    uint32_t timestamp = 0;
    // The card's scheduling state. A word that has never been reviewed keeps
    // State::New, which the scheduler treats as due immediately.
    study::CardState card;
    // The button pressed on the last review (0 when never reviewed).
    uint8_t lastRating = 0;
  };

  // Reads the index into `entries`. False when the file does not exist yet
  // (first run) or cannot be parsed; `entries` is empty in that case.
  static bool load(std::vector<Entry>& entries);

  // Appends one word, writes its long text to disk and schedules it as a new
  // card. Returns AlreadyPresent when the headword is already saved and Failed
  // when the word is empty or the files could not be written.
  static AddResult add(const std::string& headword, const std::string& definition, const std::string& sentence,
                       const std::string& book, uint32_t timestamp);

  // Removes one word and its long text. False when it was not saved.
  static bool remove(const std::string& headword);

  // True when the headword is already in the notebook.
  static bool contains(const std::string& headword);

  // Reads one word's sentence and full definition. False when the word is not
  // saved.
  static bool loadText(const std::string& headword, std::string& sentence, std::string& definition);

  // Stores the card's new scheduling state after a review.
  static bool saveCard(const std::string& headword, const study::CardState& card, uint8_t lastRating);

  // Exports the whole notebook as an Anki-importable tab-separated deck at
  // `path` (normally /vocabulary.tsv on the SD card root, easy to copy over
  // USB). Returns the number of cards written, or -1 on failure. Each entry
  // carries Word / Meaning / Sentence / Book; tabs and line breaks inside a
  // field are replaced by spaces so every card stays on one line.
  static int exportAnki(const std::string& path);

  // How many of `entries` are due at this instant.
  static int countDue(const std::vector<Entry>& entries, int today, int nowMinute);

  // The indices of the due entries, due ones first. New cards come first, then
  // the ones whose learning step expires soonest.
  static void collectDue(const std::vector<Entry>& entries, std::vector<int>& indices, int today, int nowMinute);

  // The local day number the scheduler counts days in. 0 when the clock is not
  // trustworthy.
  static int currentDayNumber();
  // Minutes elapsed in the current local day. 0 when the clock is not
  // trustworthy.
  static int currentMinuteOfDay();

 private:
  // The file name one word's long text lives under.
  static std::string textFileName(const std::string& headword);
  // The full path of one word's long text.
  static std::string textFilePath(const std::string& headword);
  // Flattens a definition into a one-line excerpt for the list row.
  static std::string makeSummary(const std::string& definition);
};
