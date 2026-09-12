# Wine4Office on macOS (experimental)

Wine4Office can run Microsoft Word on a Mac. It builds as an x86_64 Wine and
runs under Rosetta 2, with Direct3D going through Vulkan on
[MoltenVK](https://github.com/KhronosGroup/MoltenVK).

This is early work. The Wine4Office Manager app does not support macOS yet,
so for now you build and start everything from the Terminal.

## Status

| What | Status |
|------|--------|
| Build (x86_64 under Rosetta 2) | ✅ Works |
| Office install (Office Deployment Tool, Click-to-Run, App-V) | ✅ Works |
| **Word** | ⚠️ Starts and draws its ribbon, document and sign-in dialog |
| Chinese text | ✅ Fixed in DirectWrite fallback |
| Feedback buttons in the sign-in dialog | ❌ Not drawn (SVG icons) |
| Sign-in, editing, saving | ❓ Not tested yet |
| Excel, PowerPoint, Outlook, Teams | ❓ Not tested yet |
| Wine4Office Manager app | ❌ Linux only (systemd, `.desktop` files, XDG) |

**Tested on:** Apple M4, macOS 26.6, Rosetta 2 | **License tested:** Microsoft 365 Apps (O365ProPlusRetail)

## How it differs from Linux

- **Vulkan, not OpenGL.** macOS OpenGL stops at Direct3D feature level 9_3,
  and Office's Direct2D drawing needs 10_0 or later. Wine's Vulkan renderer on
  MoltenVK gets to 11_0. It only gets there with `WINE4OFFICE_VK_RELAXED_FL=1`,
  because MoltenVK lacks three features the level check asks for: geometry
  shaders, pipeline statistics queries and cull distance. Direct2D uses none of
  them. This relaxation is an experiment.
- **Keep Office hardware acceleration on.** Do not set the Office policy
  `DisableHardwareAcceleration=1`. With it, Office composites its window in
  software ("AirSpace"), which needs Direct2D effects Wine can't run, and every
  window stays white.
- **Launch `wine` directly.** `/usr/bin/arch` is protected by System Integrity
  Protection and strips `DYLD_*` variables. The `wine` binary is already x86_64,
  so run it directly and Rosetta handles the rest.

## 1. Tools

```sh
xcode-select --install                    # Command Line Tools
softwareupdate --install-rosetta --agree-to-license
brew install bison mingw-w64 cmake pkgconf python@3.12
```

Homebrew on Apple Silicon installs arm64 libraries, which an x86_64 Wine can't
link against. Build the libraries below as x86_64 into their own directory
instead.

```sh
export W4O=$HOME/wine4office-mac          # any working directory
export DEPS=$W4O/deps                     # x86_64 libraries
export RUNNER=$W4O/runner                 # installed Wine4Office
mkdir -p $W4O/src && cd $W4O/src
```

## 2. x86_64 libraries

**FreeType** (fonts):

```sh
curl -fsSLO https://downloads.sourceforge.net/project/freetype/freetype2/2.13.3/freetype-2.13.3.tar.xz
tar xf freetype-2.13.3.tar.xz && cd freetype-2.13.3
cmake -S . -B build-x86 -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON -DCMAKE_INSTALL_PREFIX=$DEPS -DFT_DISABLE_HARFBUZZ=ON \
    -DFT_DISABLE_PNG=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON \
    -DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew
cmake --build build-x86 -j && cmake --install build-x86
# Wine's build tools load FreeType at build time; give it an absolute install name.
install_name_tool -id $DEPS/lib/libfreetype.6.dylib $DEPS/lib/libfreetype.6.dylib
cd ..
```

**GMP, Nettle and GnuTLS** (HTTPS, needed by the Office download):

```sh
curl -fsSLO https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz
curl -fsSLO https://ftp.gnu.org/gnu/nettle/nettle-3.10.2.tar.gz
curl -fsSLO https://www.gnupg.org/ftp/gcrypt/gnutls/v3.8/gnutls-3.8.13.tar.xz
for f in gmp-6.3.0.tar.xz nettle-3.10.2.tar.gz gnutls-3.8.13.tar.xz; do tar xf $f; done

arch -x86_64 /bin/bash -c "
set -e
export CC='clang -arch x86_64' CXX='clang++ -arch x86_64' \
       PKG_CONFIG_LIBDIR=$DEPS/lib/pkgconfig CPPFLAGS=-I$DEPS/include LDFLAGS=-L$DEPS/lib
(cd gmp-6.3.0 && ./configure --host=x86_64-apple-darwin --prefix=$DEPS --enable-shared --disable-static \
    && make -j && make install)
(cd nettle-3.10.2 && ./configure --prefix=$DEPS --enable-shared --disable-static \
    --disable-documentation --disable-openssl && make -j && make install)
(cd gnutls-3.8.13 && ./configure --prefix=$DEPS --enable-shared --disable-static \
    --with-included-libtasn1 --with-included-unistring --without-p11-kit --without-idn \
    --without-brotli --without-zstd --without-tpm --without-tpm2 \
    --disable-doc --disable-tests --disable-tools --disable-cxx && make -j && make install)
"
```

**MIT Kerberos** (Office's App-V registration needs the Kerberos security
package; Apple's SDK Kerberos lacks the GSSAPI extensions Wine uses):

```sh
curl -fsSLO https://kerberos.org/dist/krb5/1.21/krb5-1.21.3.tar.gz
tar xf krb5-1.21.3.tar.gz && cd krb5-1.21.3/src
arch -x86_64 /bin/bash -c "
export CC='clang -arch x86_64' CXX='clang++ -arch x86_64' PKG_CONFIG_LIBDIR=$DEPS/lib/pkgconfig
./configure --prefix=$DEPS --disable-static --enable-shared --with-crypto-impl=builtin \
    --with-tls-impl=no --without-ldap --without-tcl --without-libedit --without-readline \
    --without-keyutils --without-system-verto --disable-nls && make -j && make install
"
cd ../..
```

**MoltenVK** (Vulkan on Metal; the release dylib is universal):

```sh
curl -fsSLO https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-macos.tar
tar xf MoltenVK-macos.tar
export MVK=$W4O/src/MoltenVK/MoltenVK/dynamic/dylib/macOS
```

## 3. Build Wine4Office

```sh
git clone https://github.com/ttv20/wine4office.git $W4O/wine4office
mkdir -p $W4O/build && cd $W4O/build
arch -x86_64 /bin/bash -c "
export PATH=/opt/homebrew/opt/bison/bin:/opt/homebrew/bin:\$PATH PKG_CONFIG_LIBDIR=$DEPS/lib/pkgconfig
$W4O/wine4office/configure --enable-archs=i386,x86_64 \
    CC='clang -arch x86_64' OBJC='clang -arch x86_64' CXX='clang++ -arch x86_64' \
    FREETYPE_CFLAGS='-I$DEPS/include/freetype2' FREETYPE_LIBS='-L$DEPS/lib -lfreetype' \
    GNUTLS_CFLAGS='-I$DEPS/include' GNUTLS_LIBS='-L$DEPS/lib -lgnutls' \
    KRB5_CFLAGS='-I$DEPS/include' KRB5_LIBS='-L$DEPS/lib -lkrb5' \
    GSSAPI_CFLAGS='-I$DEPS/include' GSSAPI_LIBS='-L$DEPS/lib -lgssapi_krb5' \
    LDFLAGS='-L$MVK -L$DEPS/lib' ac_cv_lib_soname_MoltenVK=libMoltenVK.dylib
make -j\$(sysctl -n hw.ncpu) && make install prefix=$RUNNER
"
```

Check that `include/config.h` in the build directory defines
`SONAME_LIBGNUTLS`, `SONAME_LIBVULKAN`, `SONAME_LIBFREETYPE`, `SONAME_LIBKRB5`
and `SONAME_LIBGSSAPI_KRB5`. If one is missing, the matching feature is
silently disabled.

Put Wine Gecko and Wine Mono next to the runner, where the prefix setup looks
for them:

```sh
mkdir -p $RUNNER/share/wine/gecko $RUNNER/share/wine/mono
curl -fsSL -o $RUNNER/share/wine/gecko/wine-gecko-2.47.4-x86.msi \
    https://dl.winehq.org/wine/wine-gecko/2.47.4/wine-gecko-2.47.4-x86.msi
curl -fsSL -o $RUNNER/share/wine/gecko/wine-gecko-2.47.4-x86_64.msi \
    https://dl.winehq.org/wine/wine-gecko/2.47.4/wine-gecko-2.47.4-x86_64.msi
curl -fsSL -o $RUNNER/share/wine/mono/wine-mono-11.3.0-x86.msi \
    https://dl.winehq.org/wine/wine-mono/11.3.0/wine-mono-11.3.0-x86.msi
```

## 4. Install Office

`tools/macos/wine4office-mac.py` drives the same backend code as the
Wine4Office Manager, without its GUI. It creates the prefix, runs the Office
Deployment Tool and starts Office apps. The backend checks the Authenticode
signature of the Office Deployment Tool, which needs a few Python packages:

```sh
python3.12 -m venv $W4O/venv
$W4O/venv/bin/pip install signify pefile zstandard certifi

export PY=$W4O/venv/bin/python
export TOOL=$W4O/wine4office/tools/macos/wine4office-mac.py
export OPTS="--runner $RUNNER --deps $DEPS --moltenvk $MVK --prefix $HOME/.wine4office-mac"

$PY $TOOL $OPTS prefix     # create the Wine prefix (a few minutes)
$PY $TOOL $OPTS office     # download and install Microsoft 365 Apps (about 4.8 GB)
```

Pass `--product` to install a different edition, for example
`--product ProPlus2024Volume` for Office LTSC 2024. Pass `--language` for
another language, for example `--language zh-cn`.

## 5. Start Word

```sh
$PY $TOOL $OPTS run word --log $W4O/word.log
```

`run` also accepts `excel` and `powerpoint`, which are untested. To start
Wine by hand instead, use this environment:

```sh
export WINEPREFIX=$HOME/.wine4office-mac
export DYLD_FALLBACK_LIBRARY_PATH=$DEPS/lib:$MVK
export WINE_D3D_CONFIG=renderer=vulkan
export WINE4OFFICE_VK_RELAXED_FL=1
export MVK_CONFIG_LOG_LEVEL=0
$RUNNER/bin/wine "C:\\Program Files\\Microsoft Office\\root\\Office16\\WINWORD.EXE"
```

## Troubleshooting

| Symptom | Cause and fix |
|---------|---------------|
| Office windows stay white | Office hardware acceleration is off. `run` resets the `DisableHardwareAcceleration` policy to 0; by hand, delete it under `HKCU\Software\Policies\Microsoft\Office\16.0\Common\Graphics`. |
| Word quits at start, log shows `Cross-process child window Metal swapchains are not implemented` | The Mac display driver can't give another process's child window a Metal surface yet. The message names the window and both processes; please report it. |
| Direct2D fails, log shows feature level 9_3 | The OpenGL renderer is in use, or `WINE4OFFICE_VK_RELAXED_FL=1` is missing. |
| Office install fails with error 1747-45-30175 | Wine was built without Kerberos. Check `SONAME_LIBGSSAPI_KRB5` in `config.h`. |
| Download or sign-in fails with certificate errors | Wine was built without GnuTLS, or Python has no CA bundle. `wine4office-mac.py` points `SSL_CERT_FILE` at `/etc/ssl/cert.pem`. |
| `Library not loaded` for FreeType, GnuTLS or MoltenVK | `DYLD_FALLBACK_LIBRARY_PATH` is missing, or Wine was started through `arch`. |
