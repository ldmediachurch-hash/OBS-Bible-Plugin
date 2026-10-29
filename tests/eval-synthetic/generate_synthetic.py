#!/usr/bin/env python3
"""
Synthetic audio & ground-truth generator for LD Worship & Word evaluation harness.
Uses macOS `say` or Linux `espeak-ng` to generate realistic 16kHz mono audio.
"""

import os
import subprocess
import sys
import tempfile
import wave

EVENTS = [
    ("conversational", "Welcome church family, we are glad you are here with us today.", None, 2.0),
    ("scripture", "Turn with me to Romans chapter eight verse twenty-eight.", "Romans 8:28", 2.5),
    ("scripture", "verse twenty-nine", "Romans 8:29", 2.0),
    ("conversational", "Amen, let us continue in prayer.", None, 2.0),
    ("scripture", "First Corinthians thirteen from verse four.", "1 Corinthians 13:4", 2.5),
    ("conversational", "Praise God for His holy word.", None, 2.0),
    ("scripture", "Psalm one hundred and twenty-one.", "Psalms 121", 2.5),
    ("conversational", "Let us lift our voices and worship the Lord.", None, 2.0),
    ("lyrics", "Amazing grace! How sweet the sound that saved a wretch like me!", "Amazing Grace|Verse 1", 2.5),
    ("lyrics", "Twas grace that taught my heart to fear, and grace my fears relieved;", "Amazing Grace|Verse 2", 2.5),
    ("conversational", "Hallelujah, what a wonderful Savior.", None, 2.0),
    ("lyrics", "Holy, holy, holy! Lord God Almighty! Early in the morning our song shall rise to Thee;", "Holy, Holy, Holy|Verse 1", 2.5),
    ("conversational", "Let us turn back to the word of God.", None, 2.0),
    ("scripture", "The book of James chapter one verse five.", "James 1:5", 2.5),
    ("scripture", "For God so loved the world, that he gave his only begotten Son, that whosoever believeth in him should not perish, but have everlasting life.", "John 3:16", 2.5),
    ("conversational", "The Lord bless you and keep you.", None, 1.0)
]

def synthesize_clip(text, out_wav):
    if sys.platform == "darwin":
        cmd = ["say", "-o", out_wav, "--data-format=LEI16@16000", text]
        subprocess.run(cmd, check=True)
    else:
        # Fallback to espeak-ng on Linux
        cmd = ["espeak-ng", "-w", out_wav, "-s", "145", text]
        subprocess.run(cmd, check=True)

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    out_wav_path = os.path.join(script_dir, "synthetic_eval.wav")
    out_csv_path = os.path.join(script_dir, "ground_truth.csv")

    current_samples = 0
    csv_rows = []

    with wave.open(out_wav_path, "wb") as out_wf:
        out_wf.setnchannels(1)
        out_wf.setsampwidth(2)
        out_wf.setframerate(16000)

        for ev_type, text, expected, silence_after in EVENTS:
            with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
                tmp_name = tmp.name
            try:
                synthesize_clip(text, tmp_name)
                with wave.open(tmp_name, "rb") as clip_wf:
                    # Read all frames
                    frames = clip_wf.readframes(clip_wf.getnframes())
                    clip_samples = clip_wf.getnframes()
                    out_wf.writeframes(frames)
                    current_samples += clip_samples

                    # Record ground-truth time at end of spoken reference
                    time_end = current_samples / 16000.0
                    if expected:
                        csv_rows.append((round(time_end, 3), ev_type, expected))

                    # Append silence
                    silence_samples = int(silence_after * 16000)
                    silence_bytes = b"\x00\x00" * silence_samples
                    out_wf.writeframes(silence_bytes)
                    current_samples += silence_samples
            finally:
                if os.path.exists(tmp_name):
                    os.remove(tmp_name)

    # Write CSV
    with open(out_csv_path, "w", encoding="utf-8") as f:
        f.write("time_end_seconds,type,expected\n")
        for t, ev_type, expected in csv_rows:
            f.write(f"{t},{ev_type},{expected}\n")

    total_sec = current_samples / 16000.0
    print(f"Generated {out_wav_path} ({total_sec:.2f}s, {len(csv_rows)} ground-truth events)")
    print(f"Generated {out_csv_path}")

if __name__ == "__main__":
    main()
