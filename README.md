# LD Worship & Word for OBS (Scripture & Lyrics)

Scripture and song lyrics lower thirds for church livestreams, controlled from docks inside OBS. Fully offline.

By LD Media Church: *empowering church media teams to create excellence on any budget.*

## What it does

### LD Scripture
- **LD Scripture dock** (Docks → LD Scripture):
  - Search with "Rom 8:28", "1 Cor 13:4-7", "Psalm 23", or keywords like "faith hope charity".
  - Pick a translation, plus an optional second translation shown side by side.
  - Preview the verse, then **Show** and **Clear**, and **◀ ▶** to step verse by verse.
  - Service list: add the pastor's scriptures before service, save and open lists. When you reach the end of a range, ▶ moves on to the next item on the list.
  - Hotkeys (Settings → Hotkeys → "LD Scripture"): Next verse, Previous verse, Show, Clear.
- **LD Scripture Lower Third** source: add it to any scene.
  - A white (or black) card along the bottom that resizes to fit the verse and fades between verses.
  - Settings: style, top or bottom, text size, margins, translation labels, fade time, and a sample verse for positioning.
- **Bibles**:
  - KJV, WEB (World English Bible), ASV and BBE come built in. They're all public domain.
  - **Bibles → Import Bible** adds your own XML or JSON Bible files:
    - Zefania XML
    - OSIS
    - OpenSong
    - "bible / book / chapter / verse" XML
    - Common JSON layouts
  - Book names stay in the Bible's own language. For example, a Yoruba Bible shows and searches "Jòhánù 3:16", and "John 3:16" works too.

### LD Lyrics
- **LD Lyrics dock** (Docks → LD Lyrics):
  - Search your song library by title or lyrics. Double-click or press Enter to open a song.
  - Slide list showing sections (`VERSE 1 · first line…`), with clear visual section dividers.
  - Up/Down keys in the search box step through the slides.
  - Section jump buttons (`V1`, `V2`, `C`, `B`, `PC`, `T`, `E`) let you instantly jump to any part of the song during spontaneous worship.
  - **Blank** button keeps the live state while temporarily hiding the lower third during instrumental breaks or speaking.
  - **Setlist**: queue songs in order for worship, reorder by dragging, save and open setlists. Reaching the end of a song and pressing ▶ automatically rolls over to the first slide of the next song in the setlist.
  - Configurable **lines per slide** (1 to 4, default 2) automatically formats and chunks lyrics.
  - Hotkeys (Settings → Hotkeys → "LD Lyrics"): Next slide, Previous slide, Show, Clear, Blank.
- **LD Lyrics Lower Third** source:
  - Matches the scripture lower third design: elegant card with rounded corners, soft shadow, red signal dot, and clean Inter typography.
  - Displays `SONG TITLE · SECTION` as the kicker and the current slide's lines below.
  - Settings: Light/Dark theme, Top/Bottom position, text alignment (Left or Center), text size, margins, optional CCLI license chip, fade duration, and sample slide preview.
- **Song Library & Formats**:
  - Songs are stored as simple UTF-8 text files in your OBS plugin configuration directory (`songs`).
  - Native `.txt` format supports optional header (`Title:`, `Author:`, `CCLI:`, `Order:`) and `[Verse 1]`, `[Chorus]`, `[Bridge]` tags with blank lines separating slides.
  - **Songs → Import songs** supports importing native text files and OpenLyrics XML (`.xml`) files from OpenLP and other worship software.
  - **Songs → New song…** and **Edit current song…** let you paste or edit lyrics directly inside OBS.
  - Ships with public-domain sample hymns: *Amazing Grace*, *Holy, Holy, Holy*, and *It Is Well with My Soul*.

## Install
- **macOS:** open the `.pkg` from the release and restart OBS. The first time, macOS may say it's from an unidentified developer: go to System Settings → Privacy & Security, then **Open Anyway**.
- **Windows:** unzip the release into `C:\ProgramData\obs-studio\plugins\` and restart OBS.
- **Linux:** install the `.deb` package or copy `ld-scripture` into `~/.config/obs-studio/plugins/`.

Then open **Docks → LD Scripture** and **Docks → LD Lyrics**, and add the lower third sources to your sermon and worship scenes.

## Sunday workflow
1. **Worship:** Set up your worship setlist in **LD Lyrics**. During worship, double-click the opening song and click **Show**. Step through with ▶ or hotkeys. Use section buttons (e.g. `C` for Chorus) when the worship leader repeats sections. Use **Blank** during instrumental breaks.
2. **Sermon:** Add scriptures to the service list in **LD Scripture**, then double-click and click **Show**. Use ▶ to advance verses. Searching for an unexpected verse keeps what's currently on screen safe until you press **Show this**.
3. Press **Clear** when transitioning between segments.

## Copyright
Modern translations and contemporary worship songs are copyrighted. Ensure your church holds appropriate CCLI or performance licenses before broadcasting copyrighted material.

## Build from source
This uses the official [OBS plugin template](https://github.com/obsproject/obs-plugintemplate). Pushing a tag like `1.1.0` builds macOS (.pkg), Windows and Ubuntu packages with GitHub Actions and attaches them to a release.

## Licence
GPL-2.0-or-later. Inter fonts © The Inter Project Authors (SIL Open Font License, see `data/fonts/OFL.txt`).
