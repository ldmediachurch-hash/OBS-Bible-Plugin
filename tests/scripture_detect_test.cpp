// Same cases as test.mjs in the JavaScript version. Build:
//   g++ -std=c++17 -O2 -Isrc src/scripture_detect.cpp tests/scripture_detect_test.cpp -o sd_test && ./sd_test tests/kjv.tsv
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include "scripture_detect.hpp"
using namespace ldsd;

static int pass = 0, fail = 0;
static std::string join(const std::vector<std::string>& v) { std::string s = "["; for (size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + v[i]; return s + "]"; }
static void check(const std::string& name, const std::vector<std::string>& got, const std::vector<std::string>& want) {
  if (got == want) pass++; else { fail++; std::cout << "FAIL " << name << "\n   got  " << join(got) << "\n   want " << join(want) << "\n"; }
}
static std::vector<std::string> labels(const std::string& t, Context c = {}) {
  std::vector<std::string> v; for (auto& r : parseReferences(t, c)) if (r.confidence >= 0.6) v.push_back(r.label); return v;
}
static const std::string D = "\xE2\x80\x93";  // en dash in labels

int main(int argc, char** argv) {
  const std::vector<std::pair<std::string, std::vector<std::string>>> cases = {
    {"turn with me to John chapter three verse sixteen", {"John 3:16"}},
    {"John three sixteen", {"John 3:16"}},
    {"first Corinthians thirteen verse four to seven", {"1 Corinthians 13:4" + D + "7"}},
    {"1st Corinthians 13:4", {"1 Corinthians 13:4"}},
    {"1 Cor 13:4-7", {"1 Corinthians 13:4" + D + "7"}},
    {"Psalm one hundred and nineteen verse one hundred and five", {"Psalms 119:105"}},
    {"psalm one nineteen verse one hundred and five", {"Psalms 119:105"}},
    {"Psalms twenty three", {"Psalms 23"}},
    {"the twenty third Psalm", {}},
    {"Filipians four thirteen", {"Philippians 4:13"}},
    {"Philippians chapter 4 verse 13", {"Philippians 4:13"}},
    {"Romans 8 28", {"Romans 8:28"}},
    {"Romans eight verse twenty eight", {"Romans 8:28"}},
    {"the book of Jude verse three", {"Jude 3"}},
    {"Jude 24", {"Jude 24"}},
    {"Habakuk two verse three", {"Habakkuk 2:3"}},
    {"Ecclesiastics three verse one", {"Ecclesiastes 3:1"}},
    {"Revelations twenty one verse four", {"Revelation 21:4"}},
    {"song of Solomon chapter two verse four", {"Song of Solomon 2:4"}},
    {"second Timothy one seven", {"2 Timothy 1:7"}},
    {"Third John verse two", {"3 John 2"}},
    {"Deutoronomy twenty eight verse thirteen", {"Deuteronomy 28:13"}},
    {"Proverbs 3 verse 5 and 6", {"Proverbs 3:5" + D + "6"}},
    {"Isaiah 40 31", {"Isaiah 40:31"}},
    {"Acts 2 verse 38", {"Acts 2:38"}},
    {"Acts of the apostles chapter two", {"Acts 2"}},
    {"first Peter five seven", {"1 Peter 5:7"}},
    {"Second Kings chapter four", {"2 Kings 4"}},
    {"Hebrews eleven one", {"Hebrews 11:1"}},
    {"Genesis 1 1", {"Genesis 1:1"}},
    {"Matthew six verse thirty three", {"Matthew 6:33"}},
    {"Lamentations 3 22 to 23", {"Lamentations 3:22" + D + "23"}},
    {"Ezekiel thirty seven", {"Ezekiel 37"}},
    {"Psalm 27-1, the Lord is my light", {"Psalms 27:1"}},
    {"Psalm 271. The Lord is my light", {"Psalms 27:1"}},
    {"John 316 for God so loved", {"John 3:16"}},
    {"Romans 828", {"Romans 8:28"}},
    {"Psalm 119", {"Psalms 119"}},
    {"Psalm 1105", {"Psalms 110:5"}},
    {"Psalm 139 verse 23, search me, O God", {"Psalms 139:23"}},
    {"Psalm one thirty nine verse twenty three", {"Psalms 139:23"}},
    {"Psalm 138, verse 7, though I walk", {"Psalms 138:7"}},
    {"1 Corinthians 13:4-7", {"1 Corinthians 13:4" + D + "7"}},
    {"open your bible to Mark eleven", {"Mark 11"}},
    {"Turn with me to the book of Romans, chapter 8, verse 28.", {"Romans 8:28"}},
    {"Romans 8.28", {"Romans 8:28"}},
    {"John 3, 16.", {"John 3:16"}},
    {"Second Corinthians 5:17 and then Romans 12:2", {"2 Corinthians 5:17", "Romans 12:2"}},
    {"I lost my job three years ago", {}},
    {"Mark two people came to the altar", {}},
    {"John the Baptist was a voice", {}},
    {"this is a number one priority", {}},
    {"the kings of the earth", {}},
    {"Genesis 50 verse 30", {}},
    {"Jude chapter 2", {}},
    {"the Roman soldiers were 3 in number", {}},
    {"we have 2 services, at 7 and 9", {}},
    {"Daniel is 3 years old", {}},
  };
  for (auto& [t, w] : cases) check(t, labels(t), w);

  check("verse follow-up", labels("now look at verse twenty eight", {"Rom", 8, 1}), {"Romans 8:28"});
  check("range follow-up", labels("read verses 4 to 7 with me", {"1Cor", 13, 0}), {"1 Corinthians 13:4" + D + "7"});
  check("next verse", labels("the next verse says", {"Rom", 8, 28}), {"Romans 8:29"});
  check("chapter follow-up", labels("go back to chapter five verse one", {"Rom", 8, 0}), {"Romans 5:1"});
  check("no context, no follow-up", labels("verse twenty eight"), {});

  {
    int64_t clock = 0; ScriptureDetector det;
    auto ev = [&](const std::string& t, bool final) { std::vector<std::string> v; for (auto& d : det.push(t, final, clock)) v.push_back(d.status + " " + d.label); return v; };
    check("interim", ev("turn with me to romans chapter eight", false), {"preview Romans 8"});
    check("final refines", ev("turn with me to romans chapter eight verse one", true), {"confirmed Romans 8:1"});
    clock += 1000; check("unrelated speech", ev("there is therefore now no condemnation", true), {});
    clock += 1000; check("follow-up in stream", ev("and if you drop down to verse twenty eight", true), {"confirmed Romans 8:28"});
    clock += 1000; check("no repeat on next push", ev("we know that all things work together for good", true), {});
    clock += 1000; auto a = ev("turn to first", true), b = ev("corinthians thirteen verse four", true); a.insert(a.end(), b.begin(), b.end());
    check("split across pushes", a, {"confirmed 1 Corinthians 13:4"});
  }

  if (argc < 2) { std::cout << pass << " passed, " << fail << " failed (quote tests skipped: pass kjv.tsv)\n"; return fail ? 1 : 0; }
  std::vector<Verse> kjv;
  { std::ifstream f(argv[1]); std::string line;
    while (std::getline(f, line)) { std::istringstream s(line); Verse v; std::string c, n; std::getline(s, v.book, '\t'); std::getline(s, c, '\t'); std::getline(s, n, '\t'); std::getline(s, v.text); v.chapter = std::stoi(c); v.verse = std::stoi(n); kjv.push_back(v); } }
  auto t0 = std::chrono::steady_clock::now(); QuoteIndex idx(kjv);
  const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  auto q = [&](const std::string& t) { auto r = idx.search(t); return std::vector<std::string>{r.empty() ? "none" : r[0].label}; };
  check("quote KJV", q("for God so loved the world that he gave his only begotten son"), {"John 3:16"});
  check("quote modern wording", q("I can do all things through Christ who strengthens me"), {"Philippians 4:13"});
  check("quote psalm", q("the Lord is my shepherd I shall not want"), {"Psalms 23:1"});
  check("quote proverbs", q("trust in the Lord with all your heart and lean not on your own understanding"), {"Proverbs 3:5"});
  check("quote jeremiah", q("for I know the thoughts that I think toward you saith the Lord"), {"Jeremiah 29:11"});
  check("quote in the middle of talk", q("and church I want you to know that they that wait upon the Lord shall renew their strength amen"), {"Isaiah 40:31"});
  check("quote: race not to the swift", q("That's why the Bible categorically explains that the race is not to the swift, not the battle to the strong."), {"Ecclesiastes 9:11"});
  check("quote: joy of the Lord", q("the joy of the Lord is your strength"), {"Nehemiah 8:10"});
  check("quote: be still", q("be still and know that I am God"), {"Psalms 46:10"});
  check("no quote in ordinary talk", q("please switch off your phones and greet the person next to you"), {"none"});
  check("no quote in sermon talk", q("And that is why we are very quick to judge, very quick to box people. That person did not deserve the miracle."), {"none"});
  t0 = std::chrono::steady_clock::now(); for (int i = 0; i < 1000; i++) idx.search("they that wait upon the Lord shall renew their strength");
  const double qMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 1000;

  {
    int64_t c = 0; LiveScripture live(&kjv); std::vector<std::string> got;
    for (const char* chunk : {"good morning church, turn with me to", "Romans chapter eight verse one", "there is therefore now no condemnation",
         "and the Bible says the race is not to the swift", "nor the battle to the strong", "the race is not to the swift church", "look at verse twenty eight"}) {
      c += 2000; for (auto& d : live.push(chunk, true, c)) got.push_back(d.kind + " " + d.label);
    }
    check("live combined", got, {"explicit Romans 8:1", "quote Ecclesiastes 9:11", "followup Romans 8:28"});
  }
  std::cout << "\n" << pass << " passed, " << fail << " failed | quote index built in " << int(buildMs) << " ms, search " << qMs << " ms\n";
  return fail ? 1 : 0;
}
