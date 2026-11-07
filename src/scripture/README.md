# LD Worship & Word: scripture detection v2

## What's in here

| Folder | What it is |
|---|---|
| `cpp/src/` | **The engine in C++17**, for the plugin: `scripture_detect.hpp/.cpp` and `scripture_bible_structure.hpp`. No dependencies. Goes in the plugin's `src/scripture/` folder. |
| `cpp/tests/` | 88 tests, plus the KJV as `kjv.tsv` for the quote tests |
| `js/` | The same engine in JavaScript, with a test page you can open in Chrome (`node serve.mjs`) |
| `test-audio/` | The Pewbeam demo clip as 16 kHz WAV, plus the references it should detect |
| `ANTIGRAVITY_PROMPT.md` | Copy-paste instructions for Antigravity's agent: Part 1 wires in the engine, Part 2 fixes the speech-to-text |

## Results on the Pewbeam demo clip

Dr. Charles Stanley reads **Psalm 27:1, Psalm 139:23, Psalm 138:7 and Psalm 138:8**. That's the same clip Pewbeam uses to show off.

I simulated live listening with the Whisper **small** model and fed the text into this engine:

| | Current plugin method (3.5 s windows + long prompt) | Recommended method (growing window + agreement) |
|---|---|---|
| References caught | 4 of 4 | 4 of 4 |
| Delay after the verse number | 0.1–0.9 s | 0.1–0.9 s |
| Wrong detections | 1 ("Psalm 138" a moment before "verse 8") | 1 brief preview ("Psalm 131", corrected a second later) |
| Invented text in the transcript | "Genesis, Chapter, Verse.", "Verse number 13", "Song 138 verse 8", "Verse number 28, verse 7" | none |

**What this shows:**

1. **The engine works.** Fed a decent transcript, it catches every reference about a second after the verse number. That's the same as Pewbeam's own demo.
2. **Your current method makes Whisper invent text.** Short windows plus the long book-list prompt produced made-up book names and verse numbers, even with the small model. Your plugin uses **tiny.en**, which is far weaker, so expect much more of this. Along with the old regex matcher, that's the most likely cause of the 5%.
3. **The first fixes to make are the model (Part 2, step 1) and the matcher (Part 1).** The streaming changes then remove the invented text and the repeated pop-ups.

**Two limits on this test:**
- I couldn't run tiny.en here, so I haven't reproduced your 5% exactly.
- This clip is clean studio audio. A real service in a hall with a PA is harder, so the next test should be a recording from one of our churches. Send one and I'll score it the same way.

## Engine behaviour (both versions)

- **Spoken references:**
  - "first Corinthians thirteen verses four to seven"
  - "Psalm one thirty nine verse twenty three", "Psalm 27-1", "Psalm 271", "John 316", "Romans 8.28"
  - ranges, one-chapter books ("Jude 24"), numbered books ("Third John verse two")
- **Accent and transcription spellings:** "Filipians", "Habakuk", "Ecclesiastics", "Deutoronomy", "Revelations"…
- **Follow-ups:** "verse twenty eight", "verses 4 to 7", "chapter five verse one", "the next verse". These use the passage already in context.
- **Quotes with no reference:** "the joy of the Lord is your strength" → Nehemiah 8:10. Modern wording still matches the KJV.
- **Guards against false alarms:**
  - Names that are also everyday words need real evidence before they fire: "I lost my job three years ago" doesn't trigger.
  - Impossible references are rejected: Genesis 50:30, Jude 2.
  - The same verse isn't repeated within 8 seconds.
  - Quote matches are held back while a just-announced passage is being read.
- **Statuses:**
  - `preview`: from interim speech. Show it to the operator, don't put it on air.
  - `confirmed`: from final speech.
  - `refined`: the same mention became more specific, e.g. "Romans 8" → "Romans 8:28".
- **Safety and speed:** passes AddressSanitizer and UndefinedBehaviorSanitizer, plus 30,000 random-input runs with no crashes. A quote search takes about 0.002 ms, and building the KJV index takes about 0.25 s once at start-up.

## Building the C++ tests

```
g++ -std=c++17 -O2 -Icpp/src cpp/src/scripture_detect.cpp cpp/tests/scripture_detect_test.cpp -o scripture_detect_test
./scripture_detect_test cpp/tests/kjv.tsv        # 88 passed, 0 failed
```

## Using it from the plugin

```cpp
#include "scripture/scripture_detect.hpp"

// once, on the whisper worker thread
std::vector<ldsd::Verse> verses = /* from bible.cpp: {"John", 3, 16, "For God so loved..."}, ... (OSIS ids) */;
ldsd::LiveScripture live(&verses);

// every whisper pass
for (const ldsd::Ref& r : live.push(newText, isFinal, nowMs)) {
  // r.label "Romans 8:28", r.book "Rom", r.chapter 8, r.verse 28, r.verseEnd 0,
  // r.kind "explicit" | "followup" | "quote", r.status "preview" | "confirmed" | "refined", r.confidence 0..1
  QMetaObject::invokeMethod(dock, [dock, r] { dock->showSuggestion(r); }, Qt::QueuedConnection);
}
```

Not thread-safe by design: keep each `LiveScripture` on the whisper worker thread, and pass results to the UI thread.
