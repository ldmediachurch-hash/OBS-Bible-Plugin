# LD Worship & Word for OBS (Scripture & Lyrics) v1.2.0

Scripture and song lyrics lower thirds for church livestreams, controlled from docks inside OBS. Fully offline speech detection with Whisper AI.

By LD Media Church: *empowering church media teams to create excellence on any budget.*

---

## What's New in v1.2.0

### Complete Dock Redesign (Clean, Focused, High-Confidence)
- **Visual Hierarchy:** White primary **Show / On screen** button draws operator focus to the single primary action.
- **Unified Live State:** Confirmed in three places at once: the white-outlined slide with `● LIVE` badge, the "On screen" button, and the 16:9 **● ON AIR** thumbnail.
- **16:9 Mini Program Thumbnail:** True rendered preview of the lower third graphic replacing the old plain text box, giving the operator confidence in what viewers actually see.
- **Next Slide Guidance:** Next slide is outlined with a subtle dashed border so the operator knows exactly what the Next key (`Space` or `▶`) will display.
- **Less Noise:** No repeated "VERSE 1 ·" labels on every row. The section header appears once per section, and each card displays the slide's actual lines.
- **Compact Audio Row:** Audio source dropdown, 5-bar vertical signal meter, and `● Listening` status pill on one compact line. Clicking the status pill gives instant access to AI and device options.
- **Slide-in Suggestion Card:** Clean banner displaying `HEARD · 95%` (or `TENTATIVE · 80%`), detected reference, a confidence indicator bar, and single-click **Show** / **Preview** / **✕**.
- **Section Chips:** Compact `V1`, `V2`, `C`, `B` chips with the active section filled in white.
- **Numbered Setlist:** Setlist displays row numbers (1, 2, 3...) with internal drag reordering and compact icon buttons (`+`, `−`, `↑`, `↓`).
- **Sticky Bottom Control Bar:** Pinned `◀`, `On screen` (wide white primary), `Blank`, `Clear`, and `▶` buttons with a persistent keyboard hint row:
  `Space next · ⇧Space back · Enter show · B blank · Esc clear`
- **Responsive 2-Column Layout:** When the dock is wider than ~620 px, it automatically switches to a side-by-side view (Left: Library/Setlist, Right: Slides/Thumbnail/Controls).

---

### Whisper Speech AI Overhaul (Fast, Accurate, Steady)

1. **Sub-Second Latency:**
   - Window duration optimized to **3.0 s with 0.75 s hop** (configurable).
   - Set `audio_ctx` matched to window length (150 frames for 3.0 s), eliminating Whisper's default 30-second zero padding and accelerating encoder passes by 5–10×.
   - Fast greedy decoding with single-segment inference.
   - Model loaded on a background thread at startup and warmed up with a silent pass.
   - Hardware acceleration active on Apple Silicon Metal and Windows/Linux AVX2.
2. **High-Accuracy Speech Detection:**
   - Default model upgraded to **`ggml-base.en-q5_1`** for reliable comprehension of church names and accents.
   - Energy-based voice activity gating drops room noise and silent intervals before speech decoding.
   - Short neutral prompt eliminates book hallucinations during silence.
3. **LocalAgreement-2 Transcript Stabilizer:**
   - Words are committed only when confirmed across consecutive sliding windows.
   - Separate tentative tail provides early detection banners that seamlessly upgrade to committed status without duplicate popups.
4. **Comprehensive Number Normalizer:**
   - Converts number words, composites ("twenty-eight" → "28", "one hundred twenty-one" → "121"), ordinals ("first corinthians" → "1 corinthians"), and separators ("8 28" → "8:28").
   - Fuzzy / phonetic matching tolerates mishearings ("Roman" → Romans, "Jon" → John, "Phillipians" → Philippians, "Habacuc" → Habakkuk, "Revelations" → Revelation).
5. **Context Memory for Follow-on Verses:**
   - When a passage is active, preaching references like *"verse 29"*, *"the next verse"*, *"back to verse 28"*, or *"chapter 9"* automatically resolve against the current book and chapter.
   - Context expires after 5 minutes or when a new book is introduced.
   - 20-second cooldown prevents repeated suggestions for the same reference.
6. **Quote Matching Across the Bible:**
   - Full 5-gram index matches spoken Scripture quotes (e.g. *"for God so loved the world"*, *"the joy of the Lord is your strength"*, *"all things work together for good"*) directly to references.
7. **Lyrics Follow Mode:**
   - In Worship mode, detection focuses on the active song: current slide, next 2 slides, section start slides, and the next setlist song.
   - Looser audio threshold preserves sung vocals over loud stage instrumentation.
   - Slide prediction tracks next slide matches across consecutive windows.

---

## Getting the Best Detection at Church

For the most reliable live detection during Sunday services:
- **Direct Aux Feed for Preacher:** Feed the plugin the pastor's microphone directly from a mixer aux output, not a room mic or laptop microphone.
- **Worship Vocal Aux:** For worship, send a dedicated aux mix where the lead vocal is 6–10 dB louder than the band.
- **Laptop Power & Resources:** Keep the OBS streaming laptop plugged into wall power (laptops throttle CPU/GPU on battery) and close unnecessary heavy background applications.

---

## Evaluation Tool (`tools/ldww-eval`)

The repository includes a standalone offline evaluation CLI (`ldww-eval`) to test detection recall, latency, and false positive rates against ground-truth audio:

```bash
# Build with CMake option BUILD_EVAL=ON
cmake -B build -DBUILD_EVAL=ON
cmake --build build --target ldww-eval

# Run evaluation on synthetic or recorded service audio
./build/tools/ldww-eval/ldww-eval \
  --wav tests/eval-synthetic/synthetic_eval.wav \
  --truth tests/eval-synthetic/ground_truth.csv \
  --songs-dir data/songs-samples \
  --verbose
```

---

## Installation

- **macOS:** Open the `.pkg` from the [Releases](https://github.com/ldmediachurch-hash/OBS-Bible-Plugin/releases) page and restart OBS.
- **Windows:** Unzip the release package into `C:\ProgramData\obs-studio\plugins\` and restart OBS.
- **Linux:** Install the `.deb` package or copy `ld-scripture` into `~/.config/obs-studio/plugins/`.

Open **Docks → LD Worship & Word** to start using the plugin.

---

## Licence

GPL-2.0-or-later. Inter fonts © The Inter Project Authors (SIL Open Font License).
