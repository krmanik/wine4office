#!/bin/bash
# Build the prebuilt Wine4Office package for macOS.
#
#   tools/macos/build.sh [deps|wine|package|all|deps-id]
#
# The package, $W4O_WORK/out/wine4office-macos-x86_64.tar.xz, holds an x86_64
# Wine4Office runner (it runs under Rosetta 2 on Apple Silicon), its x86_64
# FreeType, GnuTLS and MIT Kerberos, MoltenVK, Wine Gecko and Mono, a
# standalone Python with the manager backend's dependencies, and
# tools/macos/wine4office-mac.py. tools/macos/install.sh installs it, and
# .github/workflows/wine4office-macos.yml runs this script on GitHub's macOS
# runners.
#
# Needs the Xcode Command Line Tools, Rosetta 2 on Apple Silicon, and Homebrew
# bison, mingw-w64, pkgconf, cmake and xz. Environment:
#   W4O_WORK     work directory (default: ~/wine4office-mac)
#   W4O_VERSION  version recorded in the package (default: date and commit)
#   JOBS         parallel jobs (default: CPU count)
set -euo pipefail

STAGE_NAME=${1:-all}
SOURCE=$(cd "$(dirname "$0")/../.." && pwd -P)
WORK=${W4O_WORK:-$HOME/wine4office-mac}

FREETYPE=freetype-2.13.3
GMP=gmp-6.3.0
NETTLE=nettle-3.10.2
GNUTLS=gnutls-3.8.13
KRB5=krb5-1.21.3
MOLTENVK=1.4.2
PYTHON=cpython-3.12.14+20260901
GECKO=2.47.4
MONO=11.3.0
# The first macOS release for Apple Silicon.
export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-11.0}
DEPS_ID=$FREETYPE-$GMP-$NETTLE-$GNUTLS-$KRB5-macos$MACOSX_DEPLOYMENT_TARGET-1

case $STAGE_NAME in
    deps-id) echo "$DEPS_ID"; exit 0 ;;
    deps|wine|package|all) ;;
    *) echo "usage: $0 [deps|wine|package|all|deps-id]" >&2; exit 2 ;;
esac

# Everything below runs as x86_64, the way Wine4Office is configured and
# built. Homebrew's arm64 tools still run natively.
if [ "$(uname -m)" != x86_64 ]; then
    W4O_BREW=${W4O_BREW:-$(brew --prefix)} exec arch -x86_64 /bin/bash "$0" "$@"
fi

BREW=${W4O_BREW:-$(brew --prefix)}
export PATH="$BREW/opt/bison/bin:$BREW/bin:$PATH"
DEPS=$WORK/deps
SRC=$WORK/src
DOWNLOADS=$WORK/downloads
BUILD=$WORK/build
STAGE=$WORK/stage/Wine4Office
OUT=$WORK/out
MVK=$SRC/moltenvk-$MOLTENVK/MoltenVK/MoltenVK/dynamic/dylib/macOS
JOBS=${JOBS:-$(sysctl -n hw.ncpu)}
COMMIT=$(git -C "$SOURCE" rev-parse HEAD 2>/dev/null || echo unknown)
VERSION=${W4O_VERSION:-$(date -u +%Y%m%d)-${COMMIT:0:10}}

fetch() {  # fetch URL SHA256 [FILE]
    local file=$DOWNLOADS/${3:-${1##*/}}
    mkdir -p "$DOWNLOADS"
    if ! echo "$2  $file" | shasum -a 256 -c --status - 2>/dev/null; then
        curl -fL --retry 3 --retry-delay 5 -o "$file" "$1"
        echo "$2  $file" | shasum -a 256 -c - || { rm -f "$file"; exit 1; }
    fi
}

build_deps() {
    if [ "$(cat "$DEPS/.id" 2>/dev/null)" = "$DEPS_ID" ]; then
        echo "x86_64 libraries are up to date in $DEPS"
        return
    fi
    rm -rf "$DEPS" "$SRC/$FREETYPE" "$SRC/$GMP" "$SRC/$NETTLE" "$SRC/$GNUTLS" "$SRC/$KRB5"
    mkdir -p "$DEPS" "$SRC"
    fetch "https://downloads.sourceforge.net/project/freetype/freetype2/${FREETYPE#*-}/$FREETYPE.tar.xz" \
        0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289
    fetch "https://ftp.gnu.org/gnu/gmp/$GMP.tar.xz" \
        a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898
    fetch "https://ftp.gnu.org/gnu/nettle/$NETTLE.tar.gz" \
        fe9ff51cb1f2abb5e65a6b8c10a92da0ab5ab6eaf26e7fc2b675c45f1fb519b5
    fetch "https://www.gnupg.org/ftp/gcrypt/gnutls/v3.8/$GNUTLS.tar.xz" \
        ffed8ec1bf09c2426d4f14aae377de4753b53e537d685e604e99a8b16ca9c97e
    fetch "https://kerberos.org/dist/krb5/1.21/$KRB5.tar.gz" \
        b7a4cd5ead67fb08b980b21abd150ff7217e85ea320c9ed0c6dadd304840ad35
    for archive in $FREETYPE.tar.xz $GMP.tar.xz $NETTLE.tar.gz $GNUTLS.tar.xz $KRB5.tar.gz; do
        tar xf "$DOWNLOADS/$archive" -C "$SRC"
    done

    # FreeType (fonts)
    cmake -S "$SRC/$FREETYPE" -B "$SRC/$FREETYPE/build-x86" -DCMAKE_OSX_ARCHITECTURES=x86_64 \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DCMAKE_INSTALL_PREFIX="$DEPS" \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON \
        -DCMAKE_IGNORE_PREFIX_PATH="$BREW;/usr/local"
    cmake --build "$SRC/$FREETYPE/build-x86" -j "$JOBS"
    cmake --install "$SRC/$FREETYPE/build-x86"
    # Wine's build tools load FreeType at build time; give it an absolute install name.
    install_name_tool -id "$DEPS/lib/libfreetype.6.dylib" "$DEPS/lib/libfreetype.6.dylib"

    # GMP, Nettle and GnuTLS (HTTPS, needed by the Office download)
    (
        export CC='clang -arch x86_64' CXX='clang++ -arch x86_64' \
            PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig" CPPFLAGS="-I$DEPS/include" LDFLAGS="-L$DEPS/lib"
        # Generic x86_64: tuning for the build CPU could use instructions Rosetta lacks.
        cd "$SRC/$GMP"
        ./configure --host=x86_64-apple-darwin --prefix="$DEPS" --enable-shared --disable-static
        make -j"$JOBS" && make install
        cd "$SRC/$NETTLE"
        ./configure --prefix="$DEPS" --enable-shared --disable-static \
            --disable-documentation --disable-openssl
        make -j"$JOBS" && make install
        cd "$SRC/$GNUTLS"
        ./configure --prefix="$DEPS" --enable-shared --disable-static \
            --with-included-libtasn1 --with-included-unistring --without-p11-kit --without-idn \
            --without-brotli --without-zstd --without-tpm --without-tpm2 \
            --disable-doc --disable-tests --disable-tools --disable-cxx
        make -j"$JOBS" && make install
    )

    # MIT Kerberos (Office's App-V registration needs the Kerberos security
    # package; Apple's SDK Kerberos lacks the GSSAPI extensions Wine uses)
    (
        export CC='clang -arch x86_64' CXX='clang++ -arch x86_64' PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig"
        cd "$SRC/$KRB5/src"
        ./configure --prefix="$DEPS" --disable-static --enable-shared --with-crypto-impl=builtin \
            --with-tls-impl=no --without-ldap --without-tcl --without-libedit --without-readline \
            --without-keyutils --without-system-verto --disable-nls
        make -j"$JOBS" && make install
    )

    mkdir -p "$DEPS/licenses"
    cp "$SRC/$FREETYPE/LICENSE.TXT" "$DEPS/licenses/freetype.txt" || true
    cp "$SRC/$GMP/COPYING.LESSERv3" "$DEPS/licenses/gmp.txt" || true
    cp "$SRC/$NETTLE/COPYING.LESSERv3" "$DEPS/licenses/nettle.txt" || true
    cp "$SRC/$GNUTLS/COPYING.LESSERv2" "$DEPS/licenses/gnutls.txt" || true
    cp "$SRC/$KRB5/NOTICE" "$DEPS/licenses/krb5.txt" || true
    echo "$DEPS_ID" > "$DEPS/.id"
}

moltenvk() {
    [ -f "$MVK/libMoltenVK.dylib" ] && return
    fetch "https://github.com/KhronosGroup/MoltenVK/releases/download/v$MOLTENVK/MoltenVK-macos.tar" \
        f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e "MoltenVK-macos-$MOLTENVK.tar"
    mkdir -p "$SRC/moltenvk-$MOLTENVK"
    tar xf "$DOWNLOADS/MoltenVK-macos-$MOLTENVK.tar" -C "$SRC/moltenvk-$MOLTENVK"
}

build_wine() {
    [ -f "$DEPS/.id" ] || { echo "Build the x86_64 libraries first: $0 deps" >&2; exit 1; }
    moltenvk
    mkdir -p "$BUILD"
    cd "$BUILD"
    if [ ! -f Makefile ]; then
        PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig" "$SOURCE/configure" \
            --enable-archs=i386,x86_64 --disable-tests --without-x \
            CC='clang -arch x86_64' OBJC='clang -arch x86_64' CXX='clang++ -arch x86_64' \
            CFLAGS=-O2 OBJCFLAGS=-O2 CROSSCFLAGS=-O2 \
            FREETYPE_CFLAGS="-I$DEPS/include/freetype2" FREETYPE_LIBS="-L$DEPS/lib -lfreetype" \
            GNUTLS_CFLAGS="-I$DEPS/include" GNUTLS_LIBS="-L$DEPS/lib -lgnutls" \
            KRB5_CFLAGS="-I$DEPS/include" KRB5_LIBS="-L$DEPS/lib -lkrb5" \
            GSSAPI_CFLAGS="-I$DEPS/include" GSSAPI_LIBS="-L$DEPS/lib -lgssapi_krb5" \
            LDFLAGS="-L$MVK -L$DEPS/lib" ac_cv_lib_soname_MoltenVK=libMoltenVK.dylib
    fi
    # A missing library silently disables its feature; Office needs all of them.
    for soname in LIBFREETYPE LIBGNUTLS LIBKRB5 LIBGSSAPI_KRB5 'LIB(VULKAN|MOLTENVK)'; do
        grep -Eq "^#define SONAME_$soname " include/config.h ||
            { echo "configure did not find SONAME_$soname; see $BUILD/config.log" >&2; exit 1; }
    done
    make -j"$JOBS"
}

package() {
    [ -f "$BUILD/Makefile" ] || { echo "Build Wine4Office first: $0 wine" >&2; exit 1; }
    moltenvk
    rm -rf "$WORK/stage"
    mkdir -p "$STAGE/bin" "$STAGE/deps/lib" "$STAGE/tools/macos" "$STAGE/licenses" "$OUT"

    make -C "$BUILD" install-lib prefix="$STAGE/runner"
    rm -rf "$STAGE/runner/share/man" "$STAGE/runner/share/applications"

    # The prefix setup installs Gecko and Mono from beside the runner.
    fetch "https://dl.winehq.org/wine/wine-gecko/$GECKO/wine-gecko-$GECKO-x86.msi" \
        26cecc47706b091908f7f814bddb074c61beb8063318e9efc5a7f789857793d6
    fetch "https://dl.winehq.org/wine/wine-gecko/$GECKO/wine-gecko-$GECKO-x86_64.msi" \
        e590b7d988a32d6aa4cf1d8aa3aa3d33766fdd4cf4c89c2dcc2095ecb28d066f
    fetch "https://dl.winehq.org/wine/wine-mono/$MONO/wine-mono-$MONO-x86.msi" \
        df2dfc1665c2511882e7cabd56eafd0c0a3d94e5a7e86f969277f6c189d418d3
    mkdir -p "$STAGE/runner/share/wine/gecko" "$STAGE/runner/share/wine/mono"
    cp "$DOWNLOADS/wine-gecko-$GECKO-x86.msi" "$DOWNLOADS/wine-gecko-$GECKO-x86_64.msi" \
        "$STAGE/runner/share/wine/gecko/"
    cp "$DOWNLOADS/wine-mono-$MONO-x86.msi" "$STAGE/runner/share/wine/mono/"

    # Wine loads these by file name through DYLD_FALLBACK_LIBRARY_PATH; point
    # their references to each other away from $DEPS.
    cp -a "$DEPS"/lib/*.dylib "$STAGE/deps/lib/"
    cp "$MVK/libMoltenVK.dylib" "$STAGE/deps/lib/"
    for lib in "$STAGE"/deps/lib/*.dylib; do
        [ -L "$lib" ] && continue
        [ "${lib##*/}" = libMoltenVK.dylib ] && continue
        chmod u+w "$lib"
        install_name_tool -id "@loader_path/${lib##*/}" "$lib" 2>/dev/null
        otool -L "$lib" | awk -v deps="$DEPS/lib/" 'NR > 1 && index($1, deps) == 1 { print $1 }' |
            while read -r dep; do
                install_name_tool -change "$dep" "@loader_path/${dep##*/}" "$lib" 2>/dev/null
            done
        # install_name_tool invalidates the ad-hoc signature.
        codesign --force --sign - "$lib" 2>/dev/null
    done

    # Python for tools/macos/wine4office-mac.py and the manager backend.
    fetch "https://github.com/astral-sh/python-build-standalone/releases/download/${PYTHON#*+}/${PYTHON/+/%2B}-x86_64-apple-darwin-install_only_stripped.tar.gz" \
        65b195c9cedc1fef6767f044f9822069adbd1bd9204d424ece4628776fdc04bb \
        "$PYTHON-x86_64-apple-darwin-install_only_stripped.tar.gz"
    tar xzf "$DOWNLOADS/$PYTHON-x86_64-apple-darwin-install_only_stripped.tar.gz" -C "$STAGE"
    {
        grep -iv '^pyside6' "$SOURCE/tools/wine4office-manager/requirements-gui.txt"
        grep -i '^certifi' "$SOURCE/tools/wine4office-manager/requirements-build.txt"
    } > "$WORK/requirements-macos.txt"
    "$STAGE/python/bin/python3" -m pip install --no-cache-dir --disable-pip-version-check \
        -r "$WORK/requirements-macos.txt"
    rm -rf "$STAGE"/python/lib/python3.*/{test,idlelib,turtledemo}

    rsync -a --exclude tests --exclude probes --exclude packaging --exclude __pycache__ \
        "$SOURCE/tools/wine4office-manager/" "$STAGE/tools/wine4office-manager/"
    install -m 0755 "$SOURCE/tools/macos/wine4office-mac.py" "$SOURCE/tools/macos/install.sh" \
        "$STAGE/tools/macos/"
    cat > "$STAGE/bin/wine4office" <<'EOF'
#!/bin/sh
# Wine4Office for macOS. Run "wine4office --help".
script=$0
while [ -L "$script" ]; do
    link=$(readlink "$script")
    case $link in /*) script=$link ;; *) script=$(dirname "$script")/$link ;; esac
done
root=$(cd "$(dirname "$script")/.." && pwd -P)
exec "$root/python/bin/python3" "$root/tools/macos/wine4office-mac.py" "$@"
EOF
    chmod 0755 "$STAGE/bin/wine4office"

    echo "$VERSION" > "$STAGE/VERSION"
    cp "$SOURCE/LICENSE" "$SOURCE/COPYING.LIB" "$STAGE/licenses/"
    cp "$DEPS"/licenses/* "$STAGE/licenses/" 2>/dev/null || true
    repository=${GITHUB_REPOSITORY:+https://github.com/$GITHUB_REPOSITORY}
    cat > "$STAGE/README.txt" <<EOF
Wine4Office for macOS $VERSION (experimental)

Built from ${repository:-$(git -C "$SOURCE" remote get-url origin 2>/dev/null || echo "the Wine4Office source")}
at commit $COMMIT by tools/macos/build.sh. See MACOS.md there.

Contents and licenses:
  runner/   Wine4Office (Wine), LGPL 2.1 or later; licenses/LICENSE, COPYING.LIB
            Wine Gecko $GECKO (MPL 2.0) and Wine Mono $MONO (MIT and others)
  deps/     ${FREETYPE/-/ } (FreeType License), ${GMP/-/ } and ${NETTLE/-/ } (LGPL 3),
            ${GNUTLS/-/ } (LGPL 2.1), MIT Kerberos ${KRB5#*-} (MIT license),
            MoltenVK $MOLTENVK (Apache 2.0)
  python/   Python ${PYTHON%%+*} from python-build-standalone (PSF License),
            certifi (MPL 2.0), pefile (MIT), signify (MIT), zstandard (BSD)
  tools/    Wine4Office manager backend and the macOS command line tool
EOF

    # The package must not load anything from Homebrew or this machine.
    local bad
    bad=$(find "$STAGE/runner" "$STAGE/deps" -type f \( -name '*.so' -o -name '*.dylib' -o -perm -u+x \) \
              -exec otool -L {} + 2>/dev/null | grep -E "^[[:space:]]+(/opt/homebrew|/usr/local|$WORK)" || true)
    if [ -n "$bad" ]; then
        echo "The package links to libraries outside itself:" >&2
        echo "$bad" >&2
        exit 1
    fi
    DYLD_FALLBACK_LIBRARY_PATH="$STAGE/deps/lib" "$STAGE/runner/bin/wine" --version
    "$STAGE/bin/wine4office" products >/dev/null

    local tarball=$OUT/wine4office-macos-x86_64.tar.xz
    rm -f "$tarball" "$tarball.sha256"
    if command -v xz >/dev/null; then
        COPYFILE_DISABLE=1 tar -C "$WORK/stage" -cf - Wine4Office | xz -T0 -9 > "$tarball"
    else
        COPYFILE_DISABLE=1 tar -C "$WORK/stage" -cJf "$tarball" Wine4Office
    fi
    (cd "$OUT" && shasum -a 256 "${tarball##*/}" > "${tarball##*/}.sha256")
    install -m 0755 "$SOURCE/tools/macos/install.sh" "$OUT/install.sh"
    echo "Package: $tarball ($(du -h "$tarball" | cut -f1), version $VERSION)"
}

case $STAGE_NAME in
    deps) build_deps ;;
    wine) build_wine ;;
    package) package ;;
    all) build_deps; build_wine; package ;;
esac
