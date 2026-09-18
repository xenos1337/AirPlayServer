# MirrorSim FFmpeg Build

The checked-in Windows import libraries, DLLs, and public headers come from
FFmpeg 8.1.2. They are minimal LGPL-2.1-or-later builds with only the AAC and
H.264 decoders required by the MirrorSim receiver.

Rebuild either architecture from the repository root in a MinGW/MSYS2 or WSL
environment:

```bash
scripts/build-ffmpeg-windows.sh x64
scripts/build-ffmpeg-windows.sh x86
```

The script downloads the official source archive, verifies its pinned SHA-256,
and disables GPL, nonfree, and version-3 components. Generated build provenance
is kept in `external/ffmpeg/licenses/FFMPEG_BUILD_INFO-*.txt`.

The MirrorSim release currently packages only the x64 runtime. The x86 build is
maintained so the receiver remains buildable for legacy Windows targets.
