// LD Worship & Word: live scripture detection. See scripture_detect.hpp.
#include "scripture_detect.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

namespace ldsd {
namespace {

// ------------------------------------------------------------------ books

struct Alias {
  std::string id;                    // empty for numbered base names
  std::vector<std::string> numbered; // ids for prefix 1, 2, 3
  std::string name, key;
  int words = 1;
};

const std::vector<std::pair<std::string, std::vector<std::string>>>& aliasTable() {
  static const std::vector<std::pair<std::string, std::vector<std::string>>> t = {
      {"Gen", {"genesis", "gen"}}, {"Exod", {"exodus", "exod", "exo"}}, {"Lev", {"leviticus", "lev"}}, {"Num", {"numbers", "num"}},
      {"Deut", {"deuteronomy", "deut"}}, {"Josh", {"joshua", "josh"}}, {"Judg", {"judges", "judg"}}, {"Ruth", {"ruth"}},
      {"Ezra", {"ezra"}}, {"Neh", {"nehemiah", "neh"}}, {"Esth", {"esther", "esth"}}, {"Job", {"job"}},
      {"Ps", {"psalm", "psalms", "ps", "psa", "psalter"}}, {"Prov", {"proverbs", "proverb", "prov"}},
      {"Eccl", {"ecclesiastes", "eccl", "eccles", "qoheleth"}},
      {"Song", {"song of solomon", "songs of solomon", "song of songs", "solomons song", "canticles", "song"}},
      {"Isa", {"isaiah", "isa"}}, {"Jer", {"jeremiah", "jer"}}, {"Lam", {"lamentations", "lam"}}, {"Ezek", {"ezekiel", "ezek", "eze"}},
      {"Dan", {"daniel", "dan"}}, {"Hos", {"hosea", "hos"}}, {"Joel", {"joel"}}, {"Amos", {"amos"}}, {"Obad", {"obadiah", "obad"}},
      {"Jonah", {"jonah"}}, {"Mic", {"micah", "mic"}}, {"Nah", {"nahum", "nah"}}, {"Hab", {"habakkuk", "hab"}},
      {"Zeph", {"zephaniah", "zeph"}}, {"Hag", {"haggai", "hag"}}, {"Zech", {"zechariah", "zech"}}, {"Mal", {"malachi", "mal"}},
      {"Matt", {"matthew", "matt", "mat"}}, {"Mark", {"mark"}}, {"Luke", {"luke"}}, {"John", {"john"}},
      {"Acts", {"acts", "acts of the apostles", "the acts"}}, {"Rom", {"romans", "rom"}}, {"Gal", {"galatians", "gal"}},
      {"Eph", {"ephesians", "eph"}}, {"Phil", {"philippians", "phil"}}, {"Col", {"colossians", "col"}}, {"Titus", {"titus"}},
      {"Phlm", {"philemon", "phlm"}}, {"Heb", {"hebrews", "heb"}}, {"Jas", {"james", "jas"}}, {"Jude", {"jude"}},
      {"Rev", {"revelation", "revelations", "rev", "the revelation"}},
  };
  return t;
}
const std::vector<std::pair<std::string, std::vector<std::string>>>& numberedTable() {
  static const std::vector<std::pair<std::string, std::vector<std::string>>> t = {
      {"samuel", {"1Sam", "2Sam"}}, {"sam", {"1Sam", "2Sam"}}, {"kings", {"1Kgs", "2Kgs"}}, {"king", {"1Kgs", "2Kgs"}},
      {"chronicles", {"1Chr", "2Chr"}}, {"chron", {"1Chr", "2Chr"}}, {"corinthians", {"1Cor", "2Cor"}}, {"cor", {"1Cor", "2Cor"}},
      {"thessalonians", {"1Thess", "2Thess"}}, {"thess", {"1Thess", "2Thess"}}, {"timothy", {"1Tim", "2Tim"}}, {"tim", {"1Tim", "2Tim"}},
      {"peter", {"1Pet", "2Pet"}}, {"pet", {"1Pet", "2Pet"}}, {"john", {"1John", "2John", "3John"}},
  };
  return t;
}
// Spoken names that are also everyday words or people's names. These need stronger evidence before we fire.
const std::unordered_set<std::string> AMBIGUOUS = {"numbers", "num", "judges", "ruth", "job", "song", "daniel", "dan", "joel", "amos",
    "jonah", "mic", "matthew", "matt", "mat", "mark", "luke", "john", "acts", "the acts", "titus", "james", "jas", "jude", "col",
    "phil", "mal", "hag", "lam", "gen", "rev", "rom", "gal", "heb", "josh"};
// Words a fuzzy match must never turn into a book.
const std::unordered_set<std::string> NOT_BOOKS = {"roman", "hebrew", "jewish", "james", "number", "numbered", "judge", "judged",
    "acting", "act", "actor", "marks", "marked", "market", "look", "looks", "lucas", "jobs", "johns", "daniels", "colors",
    "collision", "genius"};
const std::vector<std::string> CUES = {"turn to", "turn with me to", "open to", "open your bible to", "book of", "gospel of",
    "gospel according to", "epistle of", "letter of", "read from", "reading from", "it says in", "look at", "go to",
    "scripture says in", "in the book of"};

std::string soundKey(const std::string& s) {
  std::string a;
  for (char c : s) if (c >= 'a' && c <= 'z') a += c;
  std::string b;
  for (size_t i = 0; i < a.size(); i++) {
    if (a[i] == 'p' && i + 1 < a.size() && a[i + 1] == 'h') { b += 'f'; i++; }
    else if (a[i] == 'c' && i + 1 < a.size() && a[i + 1] == 'k') { b += 'k'; i++; }
    else if (a[i] == 'q') b += 'k';
    else if (a[i] == 'y') b += 'i';
    else if (a[i] == 'z') b += 's';
    else b += a[i];
  }
  std::string c;
  for (char ch : b) if (c.empty() || c.back() != ch) c += ch;
  return c;
}

int lev(const std::string& a, const std::string& b) {
  if (a == b) return 0;
  const size_t m = a.size(), n = b.size();
  if (!m || !n) return int(m ? m : n);
  std::vector<int> prev(n + 1), cur(n + 1);
  for (size_t j = 0; j <= n; j++) prev[j] = int(j);
  for (size_t i = 1; i <= m; i++) {
    cur[0] = int(i);
    for (size_t j = 1; j <= n; j++)
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
    std::swap(prev, cur);
  }
  return prev[n];
}

int countWords(const std::string& s) { return 1 + int(std::count(s.begin(), s.end(), ' ')); }

const std::vector<Alias>& aliasList() {
  static const std::vector<Alias> list = [] {
    std::vector<Alias> v;
    for (auto& [id, names] : aliasTable())
      for (auto& name : names) v.push_back({id, {}, name, soundKey(name), countWords(name)});
    for (auto& [base, ids] : numberedTable()) v.push_back({"", ids, base, soundKey(base), 1});
    return v;
  }();
  return list;
}

struct AliasMatch { const Alias* a = nullptr; double score = 0; };

AliasMatch matchAlias(const std::string& phrase, int words, bool numberedOnly = false) {
  AliasMatch best;
  const std::string key = soundKey(phrase);
  for (const Alias& a : aliasList()) {
    if (a.words != words || (numberedOnly && a.numbered.empty())) continue;
    double score;
    if (a.name == phrase) score = 1;
    else {
      if (NOT_BOOKS.count(phrase) || a.name.size() < 5 || phrase.size() < 5) continue;  // short names: exact only
      const int d = lev(key, a.key), maxd = a.key.size() >= 9 ? 2 : 1;
      if (d > maxd) continue;
      score = 1 - double(d) / double(std::max(key.size(), a.key.size())) * 1.5;
    }
    if (!best.a || score > best.score) best = {&a, score};
  }
  return best;
}

struct BookHit { std::string id, alias; int start, end; double score; bool viaPrefix; };

std::string joinW(const std::vector<Token>& t, size_t a, size_t b) {
  std::string s;
  for (size_t i = a; i < b && i < t.size(); i++) { if (i > a) s += ' '; s += t[i].w; }
  return s;
}

std::vector<BookHit> findBooks(const std::vector<Token>& toks) {
  std::vector<BookHit> found;
  const int N = int(toks.size());
  for (int i = 0; i < N; i++) {
    const Token& t = toks[i];
    if (t.hasN && !(t.n >= 1 && t.n <= 3)) continue;
    // numbered book: "first corinthians", "2 timothy", "third john", "ii kings"
    if (t.hasN && i + 1 < N && !toks[i + 1].hasN) {
      AliasMatch m = matchAlias(toks[i + 1].w, 1, true);
      if (m.a && int(m.a->numbered.size()) >= t.n) {
        found.push_back({m.a->numbered[t.n - 1], m.a->name, i, i + 2, m.score, true});
        i++;
        continue;
      }
    }
    if (t.hasN) continue;
    bool hit = false;
    for (int L = 4; L >= 1 && !hit; L--) {
      if (i + L > N) continue;
      bool anyNum = false;
      for (int k = i; k < i + L; k++) anyNum |= toks[k].hasN;
      if (anyNum) continue;
      AliasMatch m = matchAlias(joinW(toks, i, i + L), L);
      if (m.a && !m.a->id.empty()) { found.push_back({m.a->id, m.a->name, i, i + L, m.score, false}); i += L - 1; hit = true; }
    }
  }
  return found;
}

// ------------------------------------------------------------------ numbers

const std::unordered_map<std::string, int> UNITS = {{"zero", 0}, {"oh", 0}, {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4},
    {"five", 5}, {"six", 6}, {"seven", 7}, {"eight", 8}, {"nine", 9}};
const std::unordered_map<std::string, int> TEENS = {{"ten", 10}, {"eleven", 11}, {"twelve", 12}, {"thirteen", 13}, {"fourteen", 14},
    {"fifteen", 15}, {"sixteen", 16}, {"seventeen", 17}, {"eighteen", 18}, {"nineteen", 19}};
const std::unordered_map<std::string, int> TENS = {{"twenty", 20}, {"thirty", 30}, {"forty", 40}, {"fourty", 40}, {"fifty", 50},
    {"sixty", 60}, {"seventy", 70}, {"eighty", 80}, {"ninety", 90}};
const std::unordered_map<std::string, int> ORD_UNITS = {{"first", 1}, {"second", 2}, {"third", 3}, {"fourth", 4}, {"fifth", 5},
    {"sixth", 6}, {"seventh", 7}, {"eighth", 8}, {"ninth", 9}};
const std::unordered_map<std::string, int> ORD_TEENS = {{"tenth", 10}, {"eleventh", 11}, {"twelfth", 12}, {"thirteenth", 13},
    {"fourteenth", 14}, {"fifteenth", 15}, {"sixteenth", 16}, {"seventeenth", 17}, {"eighteenth", 18}, {"nineteenth", 19}};
const std::unordered_map<std::string, int> ORD_TENS = {{"twentieth", 20}, {"thirtieth", 30}, {"fortieth", 40}, {"fiftieth", 50},
    {"sixtieth", 60}, {"seventieth", 70}, {"eightieth", 80}, {"ninetieth", 90}};

bool in(const std::unordered_map<std::string, int>& m, const std::string& w) { return m.count(w) > 0; }
bool isUnit(const std::string& w) { return in(UNITS, w) || in(ORD_UNITS, w); }
int unitVal(const std::string& w) { auto it = UNITS.find(w); return it != UNITS.end() ? it->second : ORD_UNITS.at(w); }
bool isDigits(const std::string& w) { return !w.empty() && std::all_of(w.begin(), w.end(), [](char c) { return c >= '0' && c <= '9'; }); }

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
  for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
  return s;
}

// ------------------------------------------------------------------ reference grammar

const std::unordered_set<std::string> FILLER = {"and", "the", "at", "in", "of", "from", "starting", "beginning"};
const std::unordered_set<std::string> RANGE = {"to", "through", "thru", "till", "until", "and", "unto", "dash"};

struct Nums {
  int chapter = 0, verse = 0, verseEnd = 0;  // 0 = none
  bool hasChapter = false, hasVerse = false;
  bool chapterKw = false, verseKw = false, colon = false, bare = false;
  int end = 0;
};

const Token* at(const std::vector<Token>& t, int i) { return i >= 0 && i < int(t.size()) ? &t[i] : nullptr; }
bool isW(const std::vector<Token>& t, int i, const char* w) { const Token* x = at(t, i); return x && x->w == w; }
bool isN(const std::vector<Token>& t, int i) { const Token* x = at(t, i); return x && x->hasN; }

bool readNumbers(const std::vector<Token>& toks, int k, bool oneChapter, Nums& r) {
  r = Nums{};
  int i = k;
  auto skip = [&] {
    while (at(toks, i) && FILLER.count(toks[i].w) && !toks[i].hasN && !(isW(toks, i + 1, "verse") && toks[i].w == "and")) i++;
  };
  skip();
  if (isW(toks, i, "chapter")) { r.chapterKw = true; i++; }
  if (isW(toks, i, "verse") && oneChapter) {
    r.verseKw = true; i++;
    if (isN(toks, i)) { r.chapter = 1; r.hasChapter = true; r.verse = toks[i].n; r.hasVerse = true; i++; }
  } else if (isN(toks, i)) {
    r.chapter = toks[i].n; r.hasChapter = true; i++;
    const int save = i;
    skip();
    if (isW(toks, i, ":") || isW(toks, i, "dash")) { r.colon = true; i++; }  // "27:1", "27-1"
    else if (isW(toks, i, "verse")) { r.verseKw = true; i++; }
    else if (isN(toks, i) && !toks[i].ord) { r.bare = true; }
    else i = save;
    if ((r.colon || r.verseKw || r.bare) && isN(toks, i)) { r.verse = toks[i].n; r.hasVerse = true; i++; }
    else if (r.colon || r.verseKw) { i = save; r.colon = r.verseKw = false; }
  } else {
    return false;
  }
  if (r.hasVerse) {  // range: "4 to 7", "4 through 7", "5 and 6"
    int j = i;
    if (isW(toks, j, "verse")) j++;
    const Token* rj = at(toks, j);
    if (rj && RANGE.count(rj->w) && !rj->hasN) {
      int q = j + 1;
      if (isW(toks, q, "verse")) q++;
      if (isN(toks, q) && toks[q].n > r.verse && (rj->w != "and" || toks[q].n == r.verse + 1)) { r.verseEnd = toks[q].n; i = q + 1; }
    }
  }
  if (oneChapter && r.hasChapter && !r.hasVerse && !r.chapterKw) { r.verse = r.chapter; r.hasVerse = true; r.chapter = 1; }  // "Jude 3"
  r.end = i;
  return true;
}

bool validate(const std::string& id, int ch, int v = 0, int ve = 0) {
  const BookInfo* b = bookById(id);
  if (!b || ch < 1 || ch > int(b->verses.size())) return false;
  const int count = b->verses[ch - 1];
  if (v && (v < 1 || v > count)) return false;
  if (ve && ve > count) return false;
  return true;
}

double round2(double x) { return std::round(x * 100) / 100; }

Ref makeRef(const std::string& book, int ch, int v, int ve, double conf, const char* kind, int start, int end) {
  Ref r;
  r.book = book; r.chapter = ch; r.verse = v; r.verseEnd = ve; r.confidence = round2(conf); r.kind = kind; r.start = start; r.end = end;
  r.label = makeLabel(r);
  return r;
}

// ------------------------------------------------------------------ quotes

const std::unordered_map<std::string, std::string> ARCHAIC = {{"thee", "you"}, {"thou", "you"}, {"thy", "your"}, {"thine", "your"},
    {"ye", "you"}, {"hath", "has"}, {"hast", "have"}, {"doth", "does"}, {"dost", "do"}, {"art", "are"}, {"shalt", "shall"},
    {"wilt", "will"}, {"unto", "to"}, {"saith", "say"}, {"spake", "spoke"}, {"didst", "did"}, {"canst", "can"},
    {"whosoever", "whoever"}, {"whatsoever", "whatever"}, {"thereof", "it"}, {"hither", "here"}, {"thither", "there"},
    {"yea", "yes"}, {"nay", "no"}, {"wherefore", "therefore"}, {"lo", "behold"}, {"which", "who"}, {"that", "who"}, {"o", "oh"}};
const std::unordered_set<std::string> STOP = [] {
  std::unordered_set<std::string> s;
  std::istringstream in(
      "a an the and or but of to in on at by for with from as is are was were be been being it its this these those "
      "i me my we us our you your he him his she her they them their who whom what so if then than not no shall will would should "
      "may might can could do does did has have had am oh all there here also unto upon into out up nor yet neither even");
  for (std::string w; in >> w;) s.insert(w);
  return s;
}();

bool endsWith(const std::string& s, const std::string& e) { return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0; }

std::string stem(std::string w) {
  auto a = ARCHAIC.find(w);
  if (a != ARCHAIC.end()) w = a->second;
  static const std::vector<std::string> keepEst = {"rest", "test", "lest", "west", "best", "feast", "beast", "least", "priest", "harvest", "forest"};
  if (w.size() > 5 && endsWith(w, "eth")) w.resize(w.size() - 3);
  else if (w.size() > 5 && endsWith(w, "est") && std::none_of(keepEst.begin(), keepEst.end(), [&](const std::string& k) { return endsWith(w, k); })) w.resize(w.size() - 3);
  else if (w.size() > 4 && endsWith(w, "ing")) w.resize(w.size() - 3);
  else if (w.size() > 4 && endsWith(w, "ed")) w.resize(w.size() - 2);
  else if (w.size() > 4 && endsWith(w, "es")) w.resize(w.size() - 2);
  else if (w.size() > 3 && endsWith(w, "s") && !endsWith(w, "ss")) w.resize(w.size() - 1);
  if (!w.empty() && (w.back() == 'e' || w.back() == 'y' || w.back() == 'i')) w.pop_back();
  return w;
}

std::vector<std::string> contentWords(const std::string& text) {
  std::string s;
  s.reserve(text.size());
  const std::string t = replaceAll(replaceAll(text, "\xE2\x80\x99", ""), "'", "");
  for (unsigned char c : t) s += (c >= 'A' && c <= 'Z') ? char(c + 32) : (c >= 'a' && c <= 'z') ? char(c) : ' ';
  std::vector<std::string> out;
  std::istringstream in(s);
  for (std::string w; in >> w;) {
    auto a = ARCHAIC.find(w);
    if (a != ARCHAIC.end()) w = a->second;
    if (STOP.count(w)) continue;
    w = stem(w);
    if (w.size() > 1) out.push_back(w);
  }
  return out;
}

}  // namespace

// ------------------------------------------------------------------ public: helpers

const BookInfo* bookById(const std::string& id) {
  static const std::unordered_map<std::string, const BookInfo*> byId = [] {
    std::unordered_map<std::string, const BookInfo*> m;
    for (const BookInfo& b : bibleStructure()) m[b.id] = &b;
    return m;
  }();
  auto it = byId.find(id);
  return it == byId.end() ? nullptr : it->second;
}

std::string makeLabel(const Ref& r) {
  const BookInfo* b = bookById(r.book);
  if (!b) return r.book;
  const std::string dash = "\xE2\x80\x93";  // en dash
  std::string s = b->name;
  if (b->verses.size() == 1 && r.verse) return s + " " + std::to_string(r.verse) + (r.verseEnd ? dash + std::to_string(r.verseEnd) : "");
  s += " " + std::to_string(r.chapter);
  if (r.verse) s += ":" + std::to_string(r.verse) + (r.verseEnd ? dash + std::to_string(r.verseEnd) : "");
  return s;
}

std::vector<Token> tokenize(const std::string& text) {
  std::string s = " ";
  {
    std::string t = text;
    for (const char* q : {"\xE2\x80\x99", "\xE2\x80\x98", "`"}) t = replaceAll(t, q, "'");
    for (const char* d : {"\xE2\x80\x93", "\xE2\x80\x94"}) t = replaceAll(t, d, "-");
    for (unsigned char c : t) s += (c >= 'A' && c <= 'Z') ? char(c + 32) : char(c);
    s += ' ';
  }
  static const std::regex reRange(R"((\d)\s*-\s*(\d))"), reColon(R"((\d)\s*:\s*(\d))"), reOrd(R"((\d+)(st|nd|rd|th)\b)"),
      rePoss(R"('s\b)"), reDL(R"((\d)([a-z]))"), reLD(R"(([a-z])(\d))"), reJunk(R"([^a-z0-9: ]+)"), reSaint(R"(\bst\b|\bsaint\b)"),
      reVerse(R"(\bverses?\b)"), reChapter(R"(\bchapters?\b)"), reVs(R"(\bvs\b|\bvv\b)");
  s = std::regex_replace(s, reRange, "$1 dash $2");
  s = std::regex_replace(s, reColon, "$1 : $2");
  s = std::regex_replace(s, reOrd, "$1");
  s = std::regex_replace(s, rePoss, "s");
  s = replaceAll(s, "'", "");
  s = std::regex_replace(s, reDL, "$1 $2");
  s = std::regex_replace(s, reLD, "$1 $2");
  std::replace(s.begin(), s.end(), '-', ' ');
  s = std::regex_replace(s, reJunk, " ");
  s = std::regex_replace(s, reSaint, " ");
  s = std::regex_replace(s, reVerse, "verse");
  s = std::regex_replace(s, reChapter, "chapter");
  s = std::regex_replace(s, reVs, "verse");

  std::vector<std::string> raw;
  {
    std::istringstream in(s);
    for (std::string w; in >> w;) raw.push_back(w);
  }
  auto R = [&](size_t j) -> const std::string& { static const std::string empty; return j < raw.size() ? raw[j] : empty; };
  std::vector<Token> out;
  for (size_t i = 0; i < raw.size();) {
    const std::string& w = raw[i];
    if (isDigits(w)) { Token t; t.w = w; t.hasN = true; t.n = w.size() > 6 ? 999999 : std::stoi(w); out.push_back(t); i++; continue; }
    if (isUnit(w) || in(TEENS, w) || in(TENS, w) || in(ORD_TEENS, w) || in(ORD_TENS, w) || w == "hundred") {
      int v = 0; size_t j = i; bool ord = false, got = false;
      if (isUnit(R(j)) && R(j + 1) == "hundred") { v = unitVal(R(j)) * 100; j += 2; got = true; }
      else if (R(j) == "hundred") { v = 100; j++; got = true; }
      if (got && R(j) == "and" && (isUnit(R(j + 1)) || in(TEENS, R(j + 1)) || in(TENS, R(j + 1)) || in(ORD_TEENS, R(j + 1)) || in(ORD_TENS, R(j + 1)))) j++;
      if (in(TENS, R(j))) {
        v += TENS.at(R(j)); j++; got = true;
        if (isUnit(R(j)) && unitVal(R(j)) > 0) { ord = in(ORD_UNITS, R(j)); v += unitVal(R(j)); j++; }
      } else if (in(ORD_TENS, R(j))) { v += ORD_TENS.at(R(j)); j++; got = true; ord = true; }
      else if (in(TEENS, R(j))) { v += TEENS.at(R(j)); j++; got = true; }
      else if (in(ORD_TEENS, R(j))) { v += ORD_TEENS.at(R(j)); j++; got = true; ord = true; }
      else if (isUnit(R(j)) && !(got && R(j) == "oh")) { ord = in(ORD_UNITS, R(j)); v += unitVal(R(j)); j++; got = true; }
      if (got) {
        Token t; t.hasN = true; t.n = v; t.ord = ord;
        for (size_t k = i; k < j; k++) { if (k > i) t.w += ' '; t.w += raw[k]; }
        out.push_back(t); i = j; continue;
      }
    }
    if (w == "ii" || w == "iii") { Token t; t.w = w; t.hasN = true; t.n = int(w.size()); t.roman = true; out.push_back(t); i++; continue; }
    Token t; t.w = w; out.push_back(t); i++;
  }
  return out;
}

// ------------------------------------------------------------------ public: parseReferences

std::vector<Ref> parseReferences(const std::string& text, Context context) { return parseReferences(tokenize(text), context); }

std::vector<Ref> parseReferences(const std::vector<Token>& toks, Context context) {
  std::vector<Ref> out;
  std::set<int> used;
  for (const BookHit& bk : findBooks(toks)) {
    const BookInfo* book = bookById(bk.id);
    const bool one = book->verses.size() == 1;
    Nums r;
    if (!readNumbers(toks, bk.end, one, r) || !r.hasChapter) continue;
    int chapter = r.chapter, verse = r.hasVerse ? r.verse : 0, verseEnd = r.verseEnd;
    // "Psalm one nineteen verse one hundred and five" -> 119:105
    if (bk.id == "Ps" && r.bare && chapter <= 1 && !validate("Ps", chapter, verse) && validate("Ps", chapter * 100 + verse)) {
      chapter = chapter * 100 + verse; verse = 0; verseEnd = 0;
      if (isW(toks, r.end, "verse") && isN(toks, r.end + 1)) { verse = toks[r.end + 1].n; r.end += 2; }
    }
    // Whisper often drops the separator: "Psalm 271" -> 27:1, "John 316" -> 3:16
    bool joinedDigits = false;
    if (!verse && !r.chapterKw && !validate(bk.id, chapter) && chapter >= 10 && chapter < 10000) {
      const std::string d = std::to_string(chapter);
      for (int cut : {int(d.size()) - 2, int(d.size()) - 1}) {
        if (cut < 1) continue;
        const int c = std::stoi(d.substr(0, size_t(cut))), v = std::stoi(d.substr(size_t(cut)));
        if (d[size_t(cut)] != '0' && validate(bk.id, c, v)) { chapter = c; verse = v; joinedDigits = true; break; }
      }
    }
    if (!validate(bk.id, chapter, verse, verseEnd)) continue;
    // evidence
    const std::string before = " " + joinW(toks, size_t(std::max(0, bk.start - 6)), size_t(bk.start)) + " ";
    bool cued = false;
    for (const std::string& c : CUES) cued |= before.find(" " + c + " ") != std::string::npos;
    const bool amb = AMBIGUOUS.count(bk.alias) && !bk.viaPrefix;
    double conf;
    if (joinedDigits) conf = amb ? (cued ? 0.8 : 0.65) : 0.8;
    else if (verse && (r.colon || r.verseKw || r.chapterKw)) conf = 0.95;
    else if (verse && one) conf = amb ? (cued ? 0.85 : 0.7) : 0.85;  // "Jude 24", "Philemon 6"
    else if (verse && r.bare) conf = amb ? (cued ? 0.85 : 0.7) : 0.9;
    else if (r.chapterKw) conf = 0.8;                                   // "Romans chapter 8"
    else conf = amb ? (cued ? 0.65 : 0) : (cued ? 0.75 : 0.6);          // "Romans 8"
    if (bk.score < 1) conf *= 0.92;                                     // fuzzy-matched name
    if (conf <= 0) continue;
    for (int i = bk.start; i < r.end; i++) used.insert(i);
    out.push_back(makeRef(bk.id, chapter, verse, verseEnd, conf, "explicit", bk.start, r.end));
    context = {bk.id, chapter, verseEnd ? verseEnd : verse};
  }
  // follow-ups with no book: "verse 28", "verses 4 to 7", "chapter 5 verse 3", "the next verse"
  const int N = int(toks.size());
  for (int i = 0; i < N; i++) {
    if (used.count(i) || !context.valid()) continue;
    const Token& t = toks[i];
    if (t.w == "chapter" && isN(toks, i + 1)) {
      Nums r;
      if (readNumbers(toks, i, false, r) && validate(context.book, r.chapter, r.hasVerse ? r.verse : 0, r.verseEnd)) {
        const int v = r.hasVerse ? r.verse : 0;
        out.push_back(makeRef(context.book, r.chapter, v, r.verseEnd, v ? 0.8 : 0.65, "followup", i, r.end));
        for (int j = i; j < r.end; j++) used.insert(j);
        context = {context.book, r.chapter, r.verseEnd ? r.verseEnd : v};
        i = r.end - 1;
      }
      continue;
    }
    if (t.w == "verse" && isN(toks, i + 1) && context.chapter) {
      const int v = toks[i + 1].n;
      int ve = 0, end = i + 2;
      const Token* re = at(toks, end);
      if (re && RANGE.count(re->w) && isN(toks, end + 1) && toks[end + 1].n > v && (re->w != "and" || toks[end + 1].n == v + 1)) { ve = toks[end + 1].n; end += 2; }
      // "...138, verse 7" with the book name lost: a number right before "verse" is the chapter, not the current one
      int ch = context.chapter, start = i;
      if (isN(toks, i - 1) && !used.count(i - 1) && toks[i - 1].n != context.chapter &&
          !(isW(toks, i - 2, "verse") || isW(toks, i - 2, "number") || isW(toks, i - 2, "chapter"))) {
        if (!validate(context.book, toks[i - 1].n, v, ve)) continue;
        ch = toks[i - 1].n; start = i - 1;
      }
      if (validate(context.book, ch, v, ve)) {
        out.push_back(makeRef(context.book, ch, v, ve, ch == context.chapter ? 0.75 : 0.7, "followup", start, end));
        for (int j = start; j < end; j++) used.insert(j);
        context.chapter = ch;
        context.verse = ve ? ve : v;
        i = end - 1;
      }
      continue;
    }
    const std::string near = joinW(toks, size_t(i), size_t(i + 3));
    auto starts = [&](const char* p) { return near.rfind(p, 0) == 0; };
    const int step = (starts("next verse") || starts("following verse")) ? 1
                     : (starts("previous verse") || starts("preceding verse") || starts("last verse")) ? -1 : 0;
    if (step && context.verse && validate(context.book, context.chapter, context.verse + step)) {
      out.push_back(makeRef(context.book, context.chapter, context.verse + step, 0, 0.7, "followup", i, i + 2));
      context.verse += step;
      i++;
    }
  }
  // "Romans chapter 8 and verse 28" / "verse 16 of John chapter 3": a chapter-only mention right next to a verse
  // in the same chapter is one reference, not two
  auto byStart = [](const Ref& a, const Ref& b) { return a.start < b.start; };
  std::stable_sort(out.begin(), out.end(), byStart);
  for (int i = int(out.size()) - 1; i >= 0; i--) {
    const Ref A = out[size_t(i)];
    if (A.verse || A.kind != "explicit") continue;
    for (size_t j = 0; j < out.size(); j++) {
      Ref& B = out[j];
      if (int(j) == i || !B.verse || B.book != A.book || B.chapter != A.chapter) continue;
      if (!((B.start >= A.end && B.start - A.end <= 2) || (A.start >= B.end && A.start - B.end <= 3))) continue;
      B.kind = "explicit"; B.confidence = std::max(B.confidence, 0.9);
      B.start = std::min(A.start, B.start); B.end = std::max(A.end, B.end);
      out.erase(out.begin() + i);
      break;
    }
  }
  std::stable_sort(out.begin(), out.end(), byStart);
  return out;
}

// ------------------------------------------------------------------ public: ScriptureDetector

void ScriptureDetector::reset() {
  words_.clear(); base_ = 0; context_ = {}; emitted_.clear(); recent_.clear(); lastFinal_.clear();
}

void ScriptureDetector::setContext(const std::string& book, int chapter, int verse, int64_t nowMs) {
  context_ = {book, chapter, verse};
  contextAt_ = nowMs;
}

std::vector<Ref> ScriptureDetector::push(const std::string& text, bool final, int64_t t) {
  if (context_.valid() && t - contextAt_ > opt_.contextTtlMs) context_ = {};
  const std::vector<Token> fresh = tokenize(text);
  std::vector<Token> toks = words_;
  toks.insert(toks.end(), fresh.begin(), fresh.end());
  std::vector<Ref> out;
  const int have = int(words_.size());
  for (Ref& r : parseReferences(toks, context_)) {
    if (r.confidence < opt_.minConfidence) continue;
    const bool isNew = r.end > have || r.start >= have;  // touches the new text
    if (!isNew) continue;
    const int64_t g = base_ + r.start;
    if (final) {
      auto prev = emitted_.find(g);
      if (prev != emitted_.end() && prev->second == r.label) continue;
      const bool refined = prev != emitted_.end();
      emitted_[g] = r.label;
      // the same verse again within a few seconds is the same mention (overlapping transcription windows)
      auto lf = lastFinal_.find(r.label);
      const bool repeat = !refined && lf != lastFinal_.end() && t - lf->second < opt_.repeatMs;
      lastFinal_[r.label] = t;
      if (repeat) continue;
      r.status = refined ? "refined" : "confirmed";
    } else {
      auto rc = recent_.find(r.label);
      if (rc != recent_.end() && t - rc->second < opt_.repeatMs) continue;
      r.status = "preview";
    }
    recent_[r.label] = t;
    context_ = {r.book, r.chapter, r.verseEnd ? r.verseEnd : r.verse};
    contextAt_ = t;
    out.push_back(r);
  }
  if (final) {
    words_.insert(words_.end(), fresh.begin(), fresh.end());
    if (words_.size() > opt_.keepWords) {
      const size_t extra = words_.size() - opt_.keepWords;
      words_.erase(words_.begin(), words_.begin() + long(extra));
      base_ += int64_t(extra);
    }
    for (auto it = emitted_.begin(); it != emitted_.end() && it->first < base_ - 200;) it = emitted_.erase(it);
  }
  for (auto it = recent_.begin(); it != recent_.end();) it = (t - it->second > 60000) ? recent_.erase(it) : std::next(it);
  for (auto it = lastFinal_.begin(); it != lastFinal_.end();) it = (t - it->second > 60000) ? lastFinal_.erase(it) : std::next(it);
  return out;
}

// ------------------------------------------------------------------ public: QuoteIndex

QuoteIndex::QuoteIndex(std::vector<Verse> verses) : verses_(std::move(verses)), size_(verses_.size(), 0) {
  for (size_t i = 0; i < verses_.size(); i++) {
    const std::vector<std::string> cw = contentWords(verses_[i].text);
    std::unordered_set<std::string> seen;
    for (size_t k = 0; k + 1 < cw.size(); k++) {
      std::string bg = cw[k] + ' ' + cw[k + 1];
      if (!seen.insert(bg).second) continue;
      post_[bg].push_back(int(i));
    }
    size_[i] = uint16_t(std::min<size_t>(seen.size(), 65535));
  }
}

std::vector<Ref> QuoteIndex::search(const std::string& text, const Options& o) const {
  std::vector<std::string> cw = contentWords(text);
  if (cw.size() > o.lastWords) cw.erase(cw.begin(), cw.end() - long(o.lastWords));
  std::vector<std::string> q;
  {
    std::unordered_set<std::string> seen;
    for (size_t k = 0; k + 1 < cw.size(); k++) { std::string bg = cw[k] + ' ' + cw[k + 1]; if (seen.insert(bg).second) q.push_back(bg); }
  }
  const double N = double(verses_.size());
  std::unordered_map<int, std::pair<int, double>> hits;  // verse -> (pairs, weight)
  for (const std::string& bg : q) {
    auto p = post_.find(bg);
    if (p == post_.end() || p->second.size() > 400) continue;
    const double w = std::log(N / double(p->second.size()));  // rare word pairs count for more
    for (int i : p->second) { auto& h = hits[i]; h.first++; h.second += w; }
  }
  std::vector<Ref> res;
  for (auto& [i, h] : hits) {
    // a match needs real weight (2 rare pairs or 3 ordinary ones) and has to cover a fair part of the verse,
    // or of what was just said, since preachers often quote half a verse inside other talk
    if (h.first < 2 || h.second < o.minWeight) continue;
    const double cover = double(h.first) / double(std::max<uint16_t>(size_[i], 1));
    if (cover < o.minCover && double(h.first) / double(q.size()) < 0.5 && h.second < 1.5 * o.minWeight) continue;
    const Verse& v = verses_[i];
    Ref r;
    r.book = v.book; r.chapter = v.chapter; r.verse = v.verse; r.text = v.text; r.kind = "quote";
    r.shared = h.first; r.weight = std::round(h.second * 10) / 10; r.score = round2(std::min(1.0, cover));
    r.start = i;  // used as a stable tie-breaker below
    res.push_back(r);
  }
  std::sort(res.begin(), res.end(), [](const Ref& a, const Ref& b) {
    if (a.weight != b.weight) return a.weight > b.weight;
    if (a.score != b.score) return a.score > b.score;
    return a.start < b.start;
  });
  if (res.size() > o.maxResults) res.resize(o.maxResults);
  for (Ref& r : res) { r.label = makeLabel(r); r.start = r.end = 0; }
  return res;
}

// ------------------------------------------------------------------ public: LiveScripture

LiveScripture::LiveScripture(const std::vector<Verse>* verses, Options o) : opt_(o), det_(o.detector) {
  if (verses && !verses->empty()) idx_ = std::make_unique<QuoteIndex>(*verses);
}

void LiveScripture::reset() { det_.reset(); tail_.clear(); quoted_.clear(); shown_.clear(); hasLastRef_ = false; }

std::vector<Ref> LiveScripture::push(const std::string& text, bool final, int64_t t) {
  std::vector<Ref> out = det_.push(text, final, t);
  for (const Ref& d : out) {
    if (d.verse) shown_.push_back({d.book, d.chapter, d.verse, d.verseEnd ? d.verseEnd : d.verse, t});
    if (d.kind != "quote") { lastRef_ = t; hasLastRef_ = true; }
  }
  shown_.erase(std::remove_if(shown_.begin(), shown_.end(), [&](const Shown& s) { return t - s.t >= opt_.quoteRepeatMs; }), shown_.end());
  if (final && idx_) {
    std::istringstream in(text);
    for (std::string w; in >> w;) tail_.push_back(w);
    if (tail_.size() > opt_.quoteWords) tail_.erase(tail_.begin(), tail_.end() - long(opt_.quoteWords));
    std::string joined;
    for (const std::string& w : tail_) { if (!joined.empty()) joined += ' '; joined += w; }
    QuoteIndex::Options qo; qo.maxResults = 1;
    std::vector<Ref> qs = idx_->search(joined, qo);
    if (!qs.empty()) {
      Ref q = qs[0];
      const bool alreadyShown = std::any_of(shown_.begin(), shown_.end(), [&](const Shown& s) {
        return s.book == q.book && s.chapter == q.chapter && q.verse >= s.a && q.verse <= s.b; });
      auto qt = quoted_.find(q.label);
      const bool recent = qt != quoted_.end() && t - qt->second < opt_.quoteRepeatMs;
      // right after a spoken reference the pastor is usually reading it (often in another translation): only very strong quotes count
      const bool reading = hasLastRef_ && t - lastRef_ < opt_.readingMs && q.weight < 30;
      if (!alreadyShown && !recent && !reading) {
        quoted_[q.label] = t;
        q.confidence = round2(std::min(0.95, 0.5 + q.weight / 60));
        q.status = "confirmed";
        out.push_back(q);
        det_.setContext(q.book, q.chapter, q.verse, t);
      }
    }
  }
  return out;
}

}  // namespace ldsd
