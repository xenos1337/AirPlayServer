# Third-Party Notices

The release runtime contains open-source components in addition to code covered
by the repository's MIT license.

## FFmpeg 8.1.2

MirrorSim's receiver uses a minimal, dynamically linked FFmpeg build for H.264
video and AAC-ELD audio decoding. It is configured with GPL, nonfree, and
version-3 components disabled. Only `avcodec`, `avutil`, `swscale`, and the AAC
and H.264 decoders are enabled.

- License: LGPL-2.1-or-later
- Source: <https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz>
- Source SHA-256: `464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c`
- Rebuild script: `scripts/build-ffmpeg-windows.sh`
- Exact build configuration: `external/ffmpeg/licenses/FFMPEG_BUILD_INFO-*.txt`

The runtime archive includes the FFmpeg license and build information. Tagged
GitHub releases also attach the exact upstream source archive and checksum.

## libplist

The receiver contains libplist code for parsing AirPlay binary property lists.
Its source is available under `AirPlayServerLib/lib/plist`, and its public API
and Windows libraries are under `external/plist`.

- License: LGPL-2.1-or-later
- Upstream project: <https://github.com/libimobiledevice/libplist>

The runtime archive includes the LGPL-2.1 license text.

## PlayFair

The AirPlay receiver contains PlayFair interoperability code under
`AirPlayServerLib/lib/playfair`.

- License: GPL-3.0
- License text: `AirPlayServerLib/lib/playfair/LICENSE.md`

The runtime archive includes the GPL-3.0 license text. The corresponding source
is included in this repository and in each GitHub release's automatically
generated source archives.

## Other vendored source

The repository also contains cryptographic, networking, SDL, ImGui, and sample
application code that is not part of MirrorSim's five-file headless runtime in
every build. Their original copyright and license notices remain with their
source files and directories.
