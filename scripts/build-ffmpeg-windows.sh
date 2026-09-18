#!/usr/bin/env bash
set -euo pipefail

FFMPEG_VERSION="8.1.2"
FFMPEG_SOURCE_URL="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"
FFMPEG_SOURCE_SHA256="464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c"

target_name="${1:-x64}"
case "${target_name}" in
    x64)
        ffmpeg_arch="x86_64"
        cross_triplet="x86_64-w64-mingw32"
        ;;
    x86)
        ffmpeg_arch="x86"
        cross_triplet="i686-w64-mingw32"
        ;;
    *)
        echo "Usage: $0 [x64|x86]" >&2
        exit 2
        ;;
esac

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work_root="${MIRRORSIM_FFMPEG_BUILD_ROOT:-${repo_root}/build/ffmpeg-windows-${target_name}}"
archive_path="${work_root}/ffmpeg-${FFMPEG_VERSION}.tar.xz"
source_dir="${work_root}/ffmpeg-${FFMPEG_VERSION}"
build_dir="${work_root}/build"
install_dir="${work_root}/install"

case "${work_root}" in
    "${repo_root}"/build/*|/tmp/*) ;;
    *)
        echo "Refusing to use FFmpeg build directory outside the repository build tree or /tmp: ${work_root}" >&2
        exit 1
        ;;
esac

mkdir -p "${work_root}"
if [[ ! -f "${archive_path}" ]]; then
    curl --fail --location --retry 3 --output "${archive_path}" "${FFMPEG_SOURCE_URL}"
fi
echo "${FFMPEG_SOURCE_SHA256}  ${archive_path}" | sha256sum --check

rm -rf "${source_dir}" "${build_dir}" "${install_dir}"
tar -xf "${archive_path}" -C "${work_root}"
mkdir -p "${build_dir}" "${install_dir}"

cross_prefix="${cross_triplet}-"
extra_cflags=""
extra_ldflags="-static-libgcc"

# A local, unprivileged MinGW extraction can be supplied for Windows/WSL
# development. CI installs the same tools normally and does not use this path.
if [[ -n "${MIRRORSIM_MINGW_ROOT:-}" ]]; then
    tool_root="$(cd "${MIRRORSIM_MINGW_ROOT}" && pwd)"
    tool_bin="${tool_root}/usr/bin"
    gcc_variant="posix"
    if [[ -x "${tool_bin}/${cross_triplet}-gcc-win32" ]]; then
        gcc_variant="win32"
    fi
    gcc_lib="$(find "${tool_root}/usr/lib/gcc/${cross_triplet}" -mindepth 1 -maxdepth 1 -type d -name "*-${gcc_variant}" -print | sort -V | tail -n 1)"
    if [[ -z "${gcc_lib}" ]]; then
        echo "Could not locate the extracted ${cross_triplet} GCC runtime." >&2
        exit 1
    fi
    target_include="${tool_root}/usr/${cross_triplet}/include"
    target_lib="${tool_root}/usr/${cross_triplet}/lib"
    tool_sysroot="${tool_root}/usr"
    ln -sf "${tool_bin}/${cross_triplet}-gcc-${gcc_variant}" "${tool_bin}/${cross_triplet}-gcc"
    export PATH="${tool_bin}:${PATH}"
    cross_prefix="${tool_bin}/${cross_triplet}-"
    extra_cflags="-B${tool_bin}/ -B${gcc_lib}/ -B${target_lib}/ --sysroot=${tool_sysroot} -isystem ${target_include}"
    extra_ldflags="-B${tool_bin}/ -B${gcc_lib}/ -B${target_lib}/ --sysroot=${tool_sysroot} -L${target_lib} -L${gcc_lib} -static-libgcc"
fi

cd "${build_dir}"
configure_args=(
    "--prefix=${install_dir}"
    "--arch=${ffmpeg_arch}"
    "--target-os=mingw32"
    "--enable-cross-compile"
    "--cross-prefix=${cross_prefix}"
    "--enable-shared"
    "--disable-static"
    "--disable-programs"
    "--disable-doc"
    "--disable-debug"
    "--disable-autodetect"
    "--disable-everything"
    "--disable-avdevice"
    "--disable-avfilter"
    "--disable-avformat"
    "--disable-swresample"
    "--disable-gpl"
    "--disable-nonfree"
    "--disable-version3"
    "--enable-avcodec"
    "--enable-avutil"
    "--enable-swscale"
    "--enable-decoder=aac"
    "--enable-decoder=h264"
    "--enable-w32threads"
    "--disable-pthreads"
    "--disable-network"
    "--disable-iconv"
    "--disable-bzlib"
    "--disable-lzma"
    "--disable-zlib"
    "--extra-cflags=${extra_cflags}"
    "--extra-ldflags=${extra_ldflags}"
)

"${source_dir}/configure" "${configure_args[@]}"
make -j"${MIRRORSIM_BUILD_JOBS:-$(nproc)}"
make install

license_dir="${install_dir}/licenses/ffmpeg"
mkdir -p "${license_dir}"
cp "${source_dir}/LICENSE.md" "${source_dir}/COPYING.LGPLv2.1" "${license_dir}/"
{
    echo "FFmpeg ${FFMPEG_VERSION}"
    echo "Target: Windows ${target_name} (${cross_triplet})"
    echo "Source: ${FFMPEG_SOURCE_URL}"
    echo "Source SHA-256: ${FFMPEG_SOURCE_SHA256}"
    echo "License configuration: LGPLv2.1-or-later; GPL, nonfree, and version-3 components disabled"
    printf 'Configure:'
    printf ' %q' "${configure_args[@]}"
    printf '\n'
} > "${install_dir}/FFMPEG_BUILD_INFO.txt"

echo "Built minimal LGPL FFmpeg for Windows at ${install_dir}"
