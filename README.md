# LD Scripture for OBS

Scripture lower thirds for church livestreams, controlled from a dock inside OBS. Fully offline.

By LD Media Church: *empowering church media teams to create excellence on any budget.*

## What it does
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

## Install
- **macOS:** open the `.pkg` from the release and restart OBS. The first time, macOS may say it's from an unidentified developer: go to System Settings → Privacy & Security, then **Open Anyway**.
- **Windows:** unzip the release into `C:\ProgramData\obs-studio\plugins\` and restart OBS.
- **Linux:** copy `ld-scripture` into `~/.config/obs-studio/plugins/`.

Then open **Docks → LD Scripture** and add the **LD Scripture Lower Third** source to your sermon scene.

## Sunday workflow
1. Before service, add the pastor's scriptures to the service list (search, then **Add**), and **Save** the list.
2. During the sermon, double-click a list item, then press **Show**. Use **▶** or your Next hotkey as each verse is read.
3. For a verse that isn't on the list, type it and press Enter. **What's on screen doesn't change until you press "Show this"**, so viewers never see you searching.
4. Press **Clear** when the pastor moves on.

## Copyright
Modern translations (NKJV, NIV, AMP, NLT and others) are copyrighted. Only import Bible files you're licensed to use and show.

## Build from source
This uses the official [OBS plugin template](https://github.com/obsproject/obs-plugintemplate). Pushing a tag like `1.0.0` builds macOS (.pkg), Windows and Ubuntu packages with GitHub Actions and attaches them to a release.

## Licence
GPL-2.0-or-later. Inter fonts © The Inter Project Authors (SIL Open Font License, see `data/fonts/OFL.txt`).
