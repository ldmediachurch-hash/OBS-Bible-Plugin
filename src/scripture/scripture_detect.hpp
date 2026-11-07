// LD Worship & Word: live scripture detection (C++17, no dependencies).
//
// Feed it speech-to-text output; it finds Bible references in it:
//   - spoken references: "turn with me to first Corinthians chapter thirteen verse four to seven"
//   - follow-ups that rely on context: "now look at verse twenty eight", "the next verse"
//   - quotes with no reference: "for God so loved the world..."            (QuoteIndex)
//
// Typical use (in the whisper worker thread):
//   ldsd::LiveScripture live(&verses);                 // verses from the plugin's Bible database (or nullptr)
//   for (auto& d : live.push(text, isFinal, nowMs)) emitSuggestion(d);
//
// Not thread-safe: call from one thread (the whisper worker), then hand results to the Qt UI thread.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "scripture_bible_structure.hpp"

namespace ldsd {

struct Token {
  std::string w;
  bool hasN = false;  // token is a number
  int n = 0;
  bool ord = false;   // ordinal word ("third")
  bool roman = false; // "ii", "iii"
};

struct Context {
  std::string book;   // empty = no context
  int chapter = 0;
  int verse = 0;      // 0 = none
  bool valid() const { return !book.empty(); }
};

struct Verse {
  std::string book;   // OSIS id: "Gen", "1Cor", "Phil", "Rev"...
  int chapter = 0;
  int verse = 0;
  std::string text;
};

struct Ref {
  std::string book;       // OSIS id
  int chapter = 0;
  int verse = 0;          // 0 = whole chapter
  int verseEnd = 0;       // 0 = single verse
  double confidence = 0;  // 0..1
  std::string kind;       // "explicit" | "followup" | "quote"
  std::string status;     // "preview" | "confirmed" | "refined"   (set by the live classes)
  std::string label;      // "1 Corinthians 13:4–7"
  int start = 0, end = 0; // token span (parseReferences only)
  // quote matches only
  std::string text;
  int shared = 0;
  double weight = 0, score = 0;
};

// ---- building blocks
std::vector<Token> tokenize(const std::string& text);
std::vector<Ref> parseReferences(const std::vector<Token>& toks, Context context = {});
std::vector<Ref> parseReferences(const std::string& text, Context context = {});
std::string makeLabel(const Ref& r);
const BookInfo* bookById(const std::string& id);

// ---- live references (interim + final speech, context, no repeats)
class ScriptureDetector {
 public:
  struct Options {
    double minConfidence = 0.6;
    size_t keepWords = 40;
    int64_t contextTtlMs = 15 * 60 * 1000;
    int64_t repeatMs = 8000;
  };
  ScriptureDetector() : ScriptureDetector(Options{}) {}
  explicit ScriptureDetector(Options o) : opt_(o) {}
  // text: the latest speech-to-text result. final=false for interim results. nowMs: any monotonic clock in ms.
  std::vector<Ref> push(const std::string& text, bool final, int64_t nowMs);
  void setContext(const std::string& book, int chapter, int verse, int64_t nowMs);
  void reset();
  const Context& context() const { return context_; }

 private:
  Options opt_;
  std::vector<Token> words_;
  int64_t base_ = 0;
  Context context_;
  int64_t contextAt_ = 0;
  std::map<int64_t, std::string> emitted_;
  std::unordered_map<std::string, int64_t> recent_;
  std::unordered_map<std::string, int64_t> lastFinal_;
};

// ---- quotes without a reference
class QuoteIndex {
 public:
  struct Options {
    size_t lastWords = 30;
    double minWeight = 15;
    double minCover = 0.25;
    size_t maxResults = 3;
  };
  explicit QuoteIndex(std::vector<Verse> verses);
  std::vector<Ref> search(const std::string& text) const { return search(text, Options{}); }
  std::vector<Ref> search(const std::string& text, const Options& o) const;
  size_t size() const { return verses_.size(); }

 private:
  std::vector<Verse> verses_;
  std::unordered_map<std::string, std::vector<int>> post_;
  std::vector<uint16_t> size_;
};

// ---- everything together
class LiveScripture {
 public:
  struct Options {
    ScriptureDetector::Options detector;
    int64_t quoteRepeatMs = 90000;
    size_t quoteWords = 30;
    int64_t readingMs = 30000;  // after a spoken reference, hold weaker quote matches this long
  };
  // verses may be nullptr (no quote search). The index copies what it needs.
  explicit LiveScripture(const std::vector<Verse>* verses) : LiveScripture(verses, Options{}) {}
  LiveScripture(const std::vector<Verse>* verses, Options o);
  std::vector<Ref> push(const std::string& text, bool final, int64_t nowMs);
  void setContext(const std::string& book, int chapter, int verse, int64_t nowMs) { det_.setContext(book, chapter, verse, nowMs); }
  void reset();

 private:
  struct Shown { std::string book; int chapter, a, b; int64_t t; };
  Options opt_;
  ScriptureDetector det_;
  std::unique_ptr<QuoteIndex> idx_;
  std::vector<std::string> tail_;
  std::unordered_map<std::string, int64_t> quoted_;
  std::vector<Shown> shown_;
  int64_t lastRef_ = 0;
  bool hasLastRef_ = false;
};

}  // namespace ldsd
