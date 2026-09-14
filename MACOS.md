# Wine4Office on macOS (experimental)

Wine4Office can run Microsoft Word on a Mac. It runs as an x86_64 Wine under
Rosetta 2, with Direct3D going through Vulkan on
[MoltenVK](https://github.com/KhronosGroup/MoltenVK).

This is early work. The Wine4Office Manager app does not support macOS yet;
an installer script sets everything up instead, using a package that GitHub
Actions builds.

## Status

| What | Status |
|------|--------|
| Prebuilt package and one-line installer | ✅ Works |
| Office install (Office Deployment Tool, Click-to-Run, App-V) | ✅ Works |
| **Word** | ⚠️ Starts and draws its ribbon, document and sign-in dialog |
| Chinese text | ✅ Fixed in DirectWrite fallback |
| Feedback buttons in the sign-in dialog | ❌ Not drawn (SVG icons) |
| Sign-in, editing, saving | ❓ Not tested yet |
| Excel, PowerPoint, Outlook, Teams | ❓ Not tested yet |
| Intel Macs | ❓ Not tested yet |
| Wine4Office Manager app | ❌ Linux only (systemd, `.desktop` files, XDG) |

**Tested on:** Apple M4, macOS 26.6, Rosetta 2 | **License tested:** Microsoft 365 Apps (O365ProPlusRetail)

## Install

You need macOS 11 or later, about 20 GB of free disk space, and a Microsoft
365 subscription or Office license. Open **Terminal** and paste:

```sh
bash -c "$(curl -fsSL https://raw.githubusercontent.com/krmanik/wine4office/mac/office/tools/macos/install.sh)"
```

The installer:

1. installs Rosetta 2 if your Mac doesn't have it yet (it may ask for your password),
2. downloads the newest Wine4Office for macOS package (a few hundred MB) from
   [GitHub Releases](https://github.com/krmanik/wine4office/releases) and checks its checksum,
3. asks which Office edition and language you want, suggesting your Mac's language,
4. creates a Wine prefix and installs Office from Microsoft (about 5 GB, 20 to
   60 minutes; the Mac is kept awake meanwhile),
5. adds Word, Excel and PowerPoint to `~/Applications/Wine4Office`.

Then open **Microsoft Word (Wine4Office)** from Launchpad or Spotlight and
sign in with your Microsoft account.

### Office editions

| Edition | `--product` | For |
|---------|-------------|-----|
| Microsoft 365 Apps for enterprise | `O365ProPlusRetail` (default) | Work or school Microsoft 365 plans; also the tested edition |
| Microsoft 365 Personal/Family | `O365HomePremRetail` | Home subscriptions |
| Microsoft 365 Apps for business | `O365BusinessRetail` | Business Basic/Standard plans |
| Office LTSC Professional Plus 2024 | `ProPlus2024Volume` | Volume licenses |
| Office LTSC Standard 2024 | `Standard2024Volume` | Volume licenses |
| Visio / Project Professional | `VisioProRetail`, `ProjectProRetail`, `VisioPro2024Volume`, `ProjectPro2024Volume` | Visio or Project on their own |

To choose without questions, download the installer and pass options:

```sh
curl -fsSLO https://raw.githubusercontent.com/krmanik/wine4office/mac/office/tools/macos/install.sh
bash install.sh --product ProPlus2024Volume --language de-de --yes
```

`--language` takes any Office language ID (`en-us`, `fr-fr`, `ja-jp`,
`zh-cn`, ...), or several separated by commas. `bash install.sh --help` lists
every option.

### Where things go

| What | Where |
|------|-------|
| Wine4Office (Wine, libraries, MoltenVK, Python) | `~/Library/Application Support/Wine4Office/app` |
| Office and its settings (the Wine prefix) | `~/.wine4office-mac` |
| App launchers | `~/Applications/Wine4Office` |
| App logs | `~/Library/Logs/Wine4Office` |

### Update and uninstall

Run the installer again to update Wine4Office; it keeps Office. To remove
Wine4Office and the app launchers, run it with `--uninstall`; it asks before
it deletes Office too.

### Command line

The package includes a `wine4office` command:

```sh
W4O="$HOME/Library/Application Support/Wine4Office/app/bin/wine4office"
"$W4O" status               # what is installed
"$W4O" run excel            # start an app (word, excel, powerpoint, ...)
"$W4O" run word --log ~/word.log
"$W4O" office --product VisioProRetail   # add another product
"$W4O" repair               # quick-repair Office
"$W4O" shortcuts            # recreate the app launchers
"$W4O" products             # list Office editions; "languages" lists languages
```

## How it differs from Linux

- **Vulkan, not OpenGL.** macOS OpenGL stops at Direct3D feature level 9_3,
  and Office's Direct2D drawing needs 10_0 or later. Wine's Vulkan renderer on
  MoltenVK gets to 11_0. It only gets there with `WINE4OFFICE_VK_RELAXED_FL=1`,
  because MoltenVK lacks three features the level check asks for: geometry
  shaders, pipeline statistics queries and cull distance. Direct2D uses none of
  them. This relaxation is an experiment. The `wine4office` command sets it.
- **Keep Office hardware acceleration on.** Do not set the Office policy
  `DisableHardwareAcceleration=1`. With it, Office composites its window in
  software ("AirSpace"), which needs Direct2D effects Wine can't run, and every
  window stays white. `wine4office run` resets the policy to 0.
- **Launch `wine` directly.** `/usr/bin/arch` is protected by System Integrity
  Protection and strips `DYLD_*` variables. The `wine` binary is already x86_64,
  so run it directly and Rosetta handles the rest.

## The prebuilt package

[`.github/workflows/wine4office-macos.yml`](.github/workflows/wine4office-macos.yml)
runs [`tools/macos/build.sh`](tools/macos/build.sh) on GitHub's Apple Silicon
macOS runner and publishes `wine4office-macos-x86_64.tar.xz`, its `.sha256`
and `install.sh` as a GitHub prerelease tagged `wine4office-macos-<version>`.
It runs when such a tag is pushed:

```sh
git tag wine4office-macos-0.1.0 && git push origin wine4office-macos-0.1.0
```

or from **Actions → Build Wine4Office for macOS → Run workflow**, which names
the release after the date and commit. GitHub only offers **Run workflow** for
workflows on the default branch; on other branches, push a tag. The
prereleases are not marked "latest", so they don't replace the Linux release
that the Linux installer downloads. The macOS installer takes the newest
release that has a macOS package. Set `WINE4OFFICE_REPO=owner/repo` to install
from another fork.

The package contains:

| Path | Contents |
|------|----------|
| `runner/` | Wine4Office built as x86_64 (`make install-lib`), with Wine Gecko and Wine Mono |
| `deps/lib/` | x86_64 FreeType, GMP, Nettle, GnuTLS and MIT Kerberos, and MoltenVK |
| `python/` | x86_64 Python from python-build-standalone with certifi, pefile, signify and zstandard |
| `tools/` | [`tools/macos/wine4office-mac.py`](tools/macos/wine4office-mac.py) and the manager backend it drives |
| `bin/wine4office` | Runs `wine4office-mac.py` with that Python |

## Build from source

`tools/macos/build.sh` is the same build the workflow runs. It needs the
Xcode Command Line Tools, Rosetta 2 and a few Homebrew tools:

```sh
xcode-select --install
softwareupdate --install-rosetta --agree-to-license
brew install bison mingw-w64 cmake pkgconf xz

git clone -b mac/office https://github.com/krmanik/wine4office.git
cd wine4office
tools/macos/build.sh        # takes a while; works in ~/wine4office-mac (set W4O_WORK to change)
tools/macos/install.sh --package ~/wine4office-mac/out/wine4office-macos-x86_64.tar.xz
```

The script runs in stages, `deps`, `wine` and `package`, which you can run
one at a time (`tools/macos/build.sh wine`); `all` is the default. What they do:

- **deps** downloads pinned, checksummed sources and builds x86_64 FreeType,
  GMP, Nettle, GnuTLS (HTTPS for the Office download) and MIT Kerberos
  (Office's App-V registration needs the Kerberos security package, and
  Apple's SDK Kerberos lacks the GSSAPI extensions Wine uses). Homebrew on
  Apple Silicon only has arm64 libraries, which an x86_64 Wine can't load.
- **wine** configures Wine4Office for x86_64 with `--enable-archs=i386,x86_64`
  against those libraries and MoltenVK, and stops if `include/config.h` lacks
  `SONAME_LIBFREETYPE`, `SONAME_LIBGNUTLS`, `SONAME_LIBKRB5`,
  `SONAME_LIBGSSAPI_KRB5` or the Vulkan library; a missing library silently
  disables its feature.
- **package** installs Wine into the package with Gecko and Mono, makes the
  libraries refer to each other by `@loader_path`, adds Python, checks that
  nothing links to Homebrew or the build directory, and writes the tarball
  and its checksum to `~/wine4office-mac/out`.

Binaries target macOS 11 (`MACOSX_DEPLOYMENT_TARGET`).

To work on `wine4office-mac.py` against a build without packaging it, install
Wine with `make -C ~/wine4office-mac/build install-lib prefix=$HOME/wine4office-mac/runner`,
put the backend's Python packages in a virtual environment
(`pip install signify pefile zstandard certifi`), and pass the paths:

```sh
python tools/macos/wine4office-mac.py --runner ~/wine4office-mac/runner \
    --deps ~/wine4office-mac/deps \
    --moltenvk ~/wine4office-mac/src/moltenvk-1.4.2/MoltenVK/MoltenVK/dynamic/dylib/macOS setup
```

To start Wine by hand, use the environment `wine4office run` sets:

```sh
APP="$HOME/Library/Application Support/Wine4Office/app"
export WINEPREFIX=$HOME/.wine4office-mac
export DYLD_FALLBACK_LIBRARY_PATH="$APP/deps/lib"
export WINE_D3D_CONFIG=renderer=vulkan
export WINE4OFFICE_VK_RELAXED_FL=1
export MVK_CONFIG_LOG_LEVEL=0
"$APP/runner/bin/wine" "C:\\Program Files\\Microsoft Office\\root\\Office16\\WINWORD.EXE"
```

## Troubleshooting

| Symptom | Cause and fix |
|---------|---------------|
| Installer says no macOS package is published | Nobody has run the macOS workflow in that repository yet; see [The prebuilt package](#the-prebuilt-package). |
| An app launcher does nothing | Check its log in `~/Library/Logs/Wine4Office`, or run `wine4office run word` in Terminal to see the error. |
| Office windows stay white | Office hardware acceleration is off. `run` resets the `DisableHardwareAcceleration` policy to 0; by hand, delete it under `HKCU\Software\Policies\Microsoft\Office\16.0\Common\Graphics`. |
| Word quits at start, log shows `Cross-process child window Metal swapchains are not implemented` | The Mac display driver can't give another process's child window a Metal surface yet. The message names the window and both processes; please report it. |
| Direct2D fails, log shows feature level 9_3 | The OpenGL renderer is in use, or `WINE4OFFICE_VK_RELAXED_FL=1` is missing. Start apps with `wine4office run`. |
| Office install fails with error 1747-45-30175 | Wine was built without Kerberos. Check `SONAME_LIBGSSAPI_KRB5` in `config.h`; `build.sh` stops when it's missing. |
| Download or sign-in fails with certificate errors | Wine was built without GnuTLS, or Python has no CA bundle. `wine4office-mac.py` points `SSL_CERT_FILE` at `/etc/ssl/cert.pem`. |
| `Library not loaded` for FreeType, GnuTLS or MoltenVK | `DYLD_FALLBACK_LIBRARY_PATH` is missing, or Wine was started through `arch`. |
