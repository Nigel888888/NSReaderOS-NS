#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/Rect.h"
#include "util/VocabularyFile.h"
#include "util/study/StudyScheduler.h"

// The review session: the words the notebook says are due, one card at a time.
//
// The question face shows the word on its own. The answer face shows the
// definition, the line the word was collected from, and the four buttons that
// schedule the next time the word comes back, each labelled with the interval
// it would produce. The scheduling itself is FSRS-5, the engine Anki runs.
class VocabularyReviewActivity final : public Activity {
 public:
  explicit VocabularyReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // One card waiting in the session.
  struct QueuedCard {
    int index = 0;      // which saved word this is
    int dueMinute = 0;  // the minute it becomes due (day * 1440 + minute)
  };

  // The cards this session will go through.
  std::vector<QueuedCard> queue;
  // Every saved word, loaded once; the queue points into it.
  std::vector<VocabularyFile::Entry> entries;
  // How many times each word has already come back in this session. A word
  // comes back at most once, so a session always ends.
  std::vector<uint8_t> returns;
  int position = 0;   // which card of the queue is showing
  int reviewed = 0;   // how many cards were answered in this session
  bool revealed = false;  // the answer face is showing
  bool finished = false;  // the queue ran out
  // The current card's text, read from disk when the card is turned over.
  std::string sentence;
  std::string definition;
  // The current card's own scheduling state, kept in step with the file.
  study::CardState card;
  std::string headword;

  // Builds the queue of due words.
  void buildQueue();
  // Puts the current card's word and scheduling state on screen.
  void loadCurrentCard();
  // Reads the current card's sentence and definition from disk.
  void loadCardText();
  // Turns the card over.
  void reveal();
  // Applies a rating and moves to the next card.
  void answer(study::Rating rating);
  // The screen box of one of the four rating buttons.
  Rect ratingButtonRect(int index) const;
  // Draws the four rating buttons with their interval previews.
  void drawRatingButtons() const;
};
