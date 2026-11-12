# LZMA decoder (LZMA SDK 26.05)

The decoder half of Igor Pavlov's LZMA SDK, unchanged, for the module's packed payloads
(`embedded_payload.cpp`, written by `pack_embedded_payloads.py`). The LZMA SDK is in the public domain.

- Source: `lzma2605.7z` from https://github.com/ip7z/7zip/releases/download/26.05/lzma2605.7z
  (linked from https://www.7-zip.org/sdk.html), SHA-256
  `668fda469b7f584f246008cf28359c66ef98d65fafb3b22a52672eebe6970c28`.
- Files, from the archive's `C/` folder (SHA-256 with the archive's CRLF line endings):

| File | SHA-256 |
| --- | --- |
| `LzmaDec.c` | `8a9d33dd01259a71b3c3cfbf645d06944bef65a152d307671372c332e9e0e022` |
| `LzmaDec.h` | `3a3b0ece1ba58601a8265a421a91b9720b39ee92ce898fadecb22f052bc3011f` |
| `7zTypes.h` | `126c3ce886391042443fcc82764d26be7aacda2298588c6ec9bb7ce8c7b47732` |
| `Precomp.h` | `88acadbc12902012872fa29337c86564bb8f9198d6be416aad6119bb537425d1` |
| `Compiler.h` | `cfafaa89ba98a7f4f38cc589533fc03facb98ae1176dfe29d58217767ed5f149` |
