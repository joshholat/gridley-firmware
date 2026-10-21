#!/usr/bin/env python3
"""Convert a locally supplied MP3 to an ESP32-ready PCM header.

Install miniaudio first: python3 -m pip install miniaudio
Usage: python3 tools/prepare_audio.py /path/to/clip.mp3
"""

import argparse
import sys
from pathlib import Path

import miniaudio


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mp3", type=Path, help="the audio file to play on BOOT double tap")
    args = parser.parse_args()

    clip = miniaudio.decode_file(
        str(args.mp3),
        output_format=miniaudio.SampleFormat.SIGNED16,
        nchannels=2,
        sample_rate=16000,
    )
    if clip.num_frames == 0 or clip.num_frames > 16000 * 15:
        parser.error("clip must be between 0 and 15 seconds long")

    samples = clip.samples
    if sys.byteorder != "little":
        samples.byteswap()
    pcm = samples.tobytes()
    output = Path(__file__).resolve().parents[1] / "WaveshareStarter" / "audio_clip.h"
    with output.open("w") as stream:
        stream.write("// Generated from a local audio file. Do not commit this file.\n")
        stream.write("#pragma once\n#include <Arduino.h>\n")
        stream.write("const uint8_t AUDIO_CLIP_PCM[] PROGMEM = {\n")
        for offset in range(0, len(pcm), 16):
            chunk = pcm[offset : offset + 16]
            stream.write("  " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",\n")
        stream.write("};\n")
        stream.write(f"constexpr size_t AUDIO_CLIP_BYTES = {len(pcm)};\n")
    print(f"Wrote {output} ({clip.num_frames / 16000:.2f} seconds)")


if __name__ == "__main__":
    main()
