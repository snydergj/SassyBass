#!/usr/bin/env python3
"""Build Billy's srmodels.bin (the speech-model partition at 0xC10000).

Keeps the VAD model from the Arduino core's own srmodels.bin and adds a set of
English WakeNet9 wake words from Espressif's esp-sr repo at the same version
the core was built with (esp-sr 2.5.3 for arduino-esp32 3.3.12). Drops the
MultiNet command model, which Billy doesn't use, to make room.

usage: pack_srmodels.py <core srmodels.bin> <esp-sr checkout> <out.bin>
"""
import os
import struct
import sys

WAKE_WORDS = [            # folder in esp-sr/model/wakenet_model; first = default
    "wn9_hiesp", "wn9_alexa", "wn9_jarvis_tts", "wn9_computer_tts",
    "wn9_heywillow_tts", "wn9_heywanda_tts", "wn9_sophia_tts", "wn9_mycroft_tts",
    "wn9_hijoy_tts", "wn9_heykira_tts3", "wn9_hijason_tts2", "wn9_heyivy_tts2",
]
KEEP_FROM_CORE = ["vadnet1_medium"]
PARTITION_SIZE = 0x3E0000


def read_pack(path):
    d = open(path, "rb").read()
    n = struct.unpack("<I", d[:4])[0]
    off, models = 4, {}
    for _ in range(n):
        name = d[off:off + 32].split(b"\0")[0].decode(); off += 32
        fn = struct.unpack("<I", d[off:off + 4])[0]; off += 4
        files = {}
        for _ in range(fn):
            f = d[off:off + 32].split(b"\0")[0].decode()
            s, l = struct.unpack("<II", d[off + 32:off + 40]); off += 40
            files[f] = d[s:s + l]
        models[name] = files
    return models


def write_pack(models, path):
    # Same layout as esp-sr/model/pack_model.py
    file_num = sum(len(f) for f in models.values())
    header_len = 4 + len(models) * 36 + file_num * 40
    head, data = struct.pack("<I", len(models)), b""
    for name, files in models.items():
        head += name.encode().ljust(32, b"\0") + struct.pack("<I", len(files))
        for fname, blob in files.items():
            head += fname.encode().ljust(32, b"\0") + struct.pack("<II", header_len + len(data), len(blob))
            data += blob
    assert len(head) == header_len
    out = head + data
    assert len(out) <= PARTITION_SIZE, f"srmodels.bin {len(out)} bytes > partition {PARTITION_SIZE}"
    open(path, "wb").write(out)
    return len(out)


def main():
    core_bin, esp_sr, out = sys.argv[1:4]
    core = read_pack(core_bin)
    models = {k: core[k] for k in KEEP_FROM_CORE if k in core}
    for w in WAKE_WORDS:
        d = os.path.join(esp_sr, "model", "wakenet_model", w)
        models[w] = {f: open(os.path.join(d, f), "rb").read() for f in sorted(os.listdir(d))}
    size = write_pack(models, out)
    print(f"srmodels.bin: {len(models)} models, {size} bytes ({size * 100 // PARTITION_SIZE}% of partition)")
    for k in models:
        print("  ", k)


if __name__ == "__main__":
    main()
