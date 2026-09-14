#!/bin/bash
# Install Wine4Office for macOS (experimental) and Microsoft Office.
#
# Downloads the prebuilt package that GitHub Actions builds
# (.github/workflows/wine4office-macos.yml), creates the Wine prefix, installs
# the Office edition you pick with the Office Deployment Tool and adds app
# launchers to ~/Applications/Wine4Office. Run it again to update.
#
#   bash -c "$(curl -fsSL https://raw.githubusercontent.com/krmanik/wine4office/mac/office/tools/macos/install.sh)"
set -euo pipefail

REPO=${WINE4OFFICE_REPO:-krmanik/wine4office}
ASSET=wine4office-macos-x86_64.tar.xz
ROOT=${WINE4OFFICE_HOME:-$HOME/Library/Application Support/Wine4Office}
APP=$ROOT/app
CLI=$APP/bin/wine4office
APPLICATIONS=$HOME/Applications/Wine4Office
DEFAULT_PRODUCT=O365ProPlusRetail
NEEDED_GB=20

usage() {
    cat <<'EOF'
Usage: install.sh [options]

Install or update Wine4Office for macOS, then Microsoft Office.

  --product ID     Office edition, e.g. O365ProPlusRetail (Microsoft 365 Apps),
                   O365HomePremRetail (Personal/Family), ProPlus2024Volume
                   (default: ask)
  --language ID    Office language, e.g. en-us, de-de, zh-cn; separate several
                   with commas (default: ask, suggesting your Mac's language)
  --no-office      install or update Wine4Office only
  --prefix DIR     Wine prefix for Office (default: ~/.wine4office-mac)
  --tag TAG        install this GitHub release instead of the newest one
  --package FILE   install a package built with tools/macos/build.sh
  --yes            don't ask questions; use the defaults
  --uninstall      remove Wine4Office and its app launchers
  -h, --help       show this help
EOF
}

PRODUCT=
LANGUAGE=
NO_OFFICE=false
PREFIX=${WINE4OFFICE_PREFIX:-$HOME/.wine4office-mac}
TAG=
PACKAGE=
ASSUME_YES=false
UNINSTALL=false
while [ $# -gt 0 ]; do
    case $1 in
        --product) PRODUCT=${2:?--product needs an Office product ID}; shift ;;
        --product=*) PRODUCT=${1#*=} ;;
        --language) LANGUAGE=${2:?--language needs a language ID}; shift ;;
        --language=*) LANGUAGE=${1#*=} ;;
        --no-office) NO_OFFICE=true ;;
        --prefix) PREFIX=${2:?--prefix needs a directory}; shift ;;
        --prefix=*) PREFIX=${1#*=} ;;
        --tag) TAG=${2:?--tag needs a release tag}; shift ;;
        --tag=*) TAG=${1#*=} ;;
        --package) PACKAGE=${2:?--package needs a file}; shift ;;
        --package=*) PACKAGE=${1#*=} ;;
        -y|--yes) ASSUME_YES=true ;;
        --uninstall) UNINSTALL=true ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done

if [ -t 1 ]; then
    BOLD=$'\033[1m' DIM=$'\033[2m' RED=$'\033[31m' GREEN=$'\033[32m' RESET=$'\033[0m'
else
    BOLD= DIM= RED= GREEN= RESET=
fi
step() { printf '\n%s==> %s%s\n' "$BOLD" "$*" "$RESET"; }
info() { printf '    %s\n' "$*"; }
fail() { printf '\n%sError:%s %s\n' "$RED" "$RESET" "$*" >&2; exit 1; }

# Questions go to the terminal even when this script arrives through a pipe.
INTERACTIVE=false
if [ "$ASSUME_YES" = false ] && (: </dev/tty) 2>/dev/null; then
    INTERACTIVE=true
fi
ask() {  # ask PROMPT DEFAULT; the answer is in $REPLY
    REPLY=
    if [ "$INTERACTIVE" = true ]; then
        printf '%s' "$1" >/dev/tty
        IFS= read -r REPLY </dev/tty || REPLY=
    fi
    [ -n "$REPLY" ] || REPLY=$2
}
confirm() {  # confirm PROMPT [DEFAULT]
    ask "$1 [$( [ "${2:-y}" = y ] && echo Y/n || echo y/N )] " "${2:-y}"
    case $REPLY in [Yy]*) return 0 ;; *) return 1 ;; esac
}

preflight() {
    [ "$(uname -s)" = Darwin ] ||
        fail "This installer is for macOS. On Linux, use install.sh in the repository root."
    [ "$(sw_vers -productVersion | cut -d. -f1)" -ge 11 ] ||
        fail "Wine4Office needs macOS 11 Big Sur or later."
    if [ "$(uname -m)" = arm64 ] && ! arch -x86_64 /usr/bin/true 2>/dev/null; then
        step "Installing Rosetta 2"
        info "Office and Wine4Office are Intel programs; Apple's Rosetta 2 runs them."
        softwareupdate --install-rosetta --agree-to-license ||
            sudo softwareupdate --install-rosetta --agree-to-license ||
            fail "Could not install Rosetta 2. Run: softwareupdate --install-rosetta"
    fi
}

office_running() {
    pgrep -f "$APP/runner/" >/dev/null 2>&1
}

stop_office() {
    office_running || return 0
    if [ "$ASSUME_YES" = false ]; then
        [ "$INTERACTIVE" = true ] || fail "Office is running. Quit it and run the installer again."
        confirm "    Office is running. Quit it now? Unsaved documents are lost." n ||
            fail "Quit Office and run the installer again."
    fi
    WINEPREFIX=$PREFIX "$APP/runner/bin/wineserver" -k 2>/dev/null || true
    local i
    for i in 1 2 3 4 5 6 7 8 9 10; do
        office_running || return 0
        sleep 1
    done
    fail "Office did not quit. Quit it and run the installer again."
}

resolve_release() {
    if [ -n "$TAG" ]; then
        URL=https://github.com/$REPO/releases/download/$TAG/$ASSET
        return
    fi
    curl -fsSL --retry 3 -H 'Accept: application/vnd.github+json' -o "$WORK/releases.json" \
        "https://api.github.com/repos/$REPO/releases?per_page=100" ||
        fail "Could not reach GitHub. Check your internet connection, or pass --tag."
    # Releases come newest first; take the first one with a macOS package.
    URL=$(tr ',{}' '\n\n\n' < "$WORK/releases.json" |
          awk -F'"' -v asset="/$ASSET" '$2 == "browser_download_url" &&
              substr($4, length($4) - length(asset) + 1) == asset { print $4; exit }')
    [ -n "$URL" ] || fail "No macOS package is published in https://github.com/$REPO/releases yet."
}

download() {
    resolve_release
    step "Downloading Wine4Office"
    info "${DIM}$URL${RESET}"
    curl -fL --retry 3 --progress-bar -o "$WORK/$ASSET" "$URL" || fail "The download failed."
    curl -fsSL --retry 3 -o "$WORK/$ASSET.sha256" "$URL.sha256" || fail "Could not download the checksum."
    [ "$(awk '{ print $1; exit }' "$WORK/$ASSET.sha256")" = "$(shasum -a 256 "$WORK/$ASSET" | awk '{ print $1 }')" ] ||
        fail "The download is damaged (checksum mismatch). Run the installer again."
    PACKAGE=$WORK/$ASSET
}

install_package() {
    step "Installing Wine4Office"
    [ -f "$PACKAGE" ] || fail "$PACKAGE does not exist."
    mkdir -p "$ROOT"
    rm -rf "$ROOT/.new"
    mkdir "$ROOT/.new"
    tar -xJf "$PACKAGE" -C "$ROOT/.new" || fail "Could not unpack $PACKAGE."
    [ -x "$ROOT/.new/Wine4Office/bin/wine4office" ] || fail "$PACKAGE is not a Wine4Office for macOS package."
    # A package downloaded with a browser is quarantined, and macOS would block its programs.
    find "$ROOT/.new" -xattrname com.apple.quarantine \
        -exec xattr -d com.apple.quarantine {} + 2>/dev/null || true
    stop_office
    rm -rf "$ROOT/.old"
    [ ! -e "$APP" ] || mv "$APP" "$ROOT/.old"
    mv "$ROOT/.new/Wine4Office" "$APP"
    rm -rf "$ROOT/.old" "$ROOT/.new"
    info "Version $(cat "$APP/VERSION") in $APP"
}

choose_product() {
    [ -z "$PRODUCT" ] || return 0
    PRODUCT=$DEFAULT_PRODUCT
    [ "$INTERACTIVE" = true ] || return 0
    local ids=() id label count=0
    step "Which Office do you want?"
    info "Pick the edition your Microsoft license covers. You sign in after the install."
    while IFS=$'\t' read -r id label; do
        ids+=("$id")
        count=$((count + 1))
        printf '    %2d) %s\n' "$count" "$label"
    done < <("$CLI" products)
    [ "$count" -gt 0 ] || fail "Could not list the Office editions."
    while :; do
        ask "    Edition [1]: " 1
        case $REPLY in
            ''|*[!0-9]*) ;;
            *) if [ "$REPLY" -ge 1 ] && [ "$REPLY" -le "$count" ]; then
                   PRODUCT=${ids[$((REPLY - 1))]}
                   return
               fi ;;
        esac
        info "Enter a number from 1 to $count."
    done
}

mac_language() {  # the Office language closest to the Mac's, from the list in $1
    local locale match
    locale=$(defaults read -g AppleLocale 2>/dev/null || echo en_US)
    locale=$(printf '%s' "${locale%%@*}" | tr 'A-Z_' 'a-z-')
    case $locale in
        zh-hant*|zh-tw|zh-hk|zh-mo) locale=zh-tw ;;
        zh*) locale=zh-cn ;;
    esac
    if printf '%s\n' "$1" | grep -qx -- "$locale"; then
        echo "$locale"
        return
    fi
    match=$(printf '%s\n' "$1" | grep -- "^${locale%%-*}-" | sed -n 1p || true)
    echo "${match:-en-us}"
}

choose_language() {
    [ -z "$LANGUAGE" ] || return 0
    local supported
    supported=$("$CLI" languages)
    LANGUAGE=$(mac_language "$supported")
    [ "$INTERACTIVE" = true ] || return 0
    step "Which language?"
    while :; do
        ask "    Office language [$LANGUAGE] (? lists them): " "$LANGUAGE"
        if [ "$REPLY" = "?" ]; then
            printf '%s\n' "$supported" | paste - - - - - - | sed 's/^/    /'
            continue
        fi
        REPLY=$(printf '%s' "$REPLY" | tr 'A-Z_' 'a-z-')
        if printf '%s\n' "$supported" | grep -qx -- "$REPLY"; then
            LANGUAGE=$REPLY
            return
        fi
        info "\"$REPLY\" is not an Office language. Type ? to list them."
    done
}

check_space() {
    local free_gb
    free_gb=$(df -Pk "$HOME" | awk 'NR == 2 { print int($4 / 1048576) }')
    [ "$free_gb" -lt "$NEEDED_GB" ] || return 0
    info "${RED}Only $free_gb GB of disk space is free.${RESET} Office needs about $NEEDED_GB GB."
    confirm "    Continue anyway?" n || exit 1
}

office_installed() {
    "$CLI" --prefix "$PREFIX" status >/dev/null 2>&1
}

setup() {
    if [ "$NO_OFFICE" = true ]; then
        step "Setting up the Wine prefix"
        "$CLI" --prefix "$PREFIX" setup --no-office
        return
    fi
    local label
    label=$("$CLI" products | awk -F'\t' -v id="$PRODUCT" '$1 == id { print $2 }')
    step "Installing ${label:-$PRODUCT} ($LANGUAGE)"
    info "Office downloads about 5 GB from Microsoft; this takes 20 to 60 minutes."
    info "Keep your Mac awake and connected to power. Office windows may appear meanwhile."
    # caffeinate keeps the Mac awake until the install finishes.
    caffeinate -i "$CLI" --prefix "$PREFIX" --product "$PRODUCT" --language "$LANGUAGE" setup
}

finish() {
    step "${GREEN}Wine4Office is ready${RESET}"
    if ls "$APPLICATIONS"/*.app >/dev/null 2>&1; then
        info "Open Office from $APPLICATIONS, Launchpad or Spotlight (type \"Word\")."
        info "Sign in with your Microsoft account when Office asks."
        if [ "$INTERACTIVE" = true ]; then open "$APPLICATIONS" || true; fi
    fi
    info "Command line:  \"$CLI\" run word"
    info "Logs:          ~/Library/Logs/Wine4Office"
    info "Update:        run this installer again"
    info "Uninstall:     run this installer with --uninstall"
}

uninstall() {
    step "Removing Wine4Office"
    [ ! -x "$APP/runner/bin/wineserver" ] || stop_office
    rm -rf "$APP" "$ROOT/.new" "$ROOT/.old" "$APPLICATIONS"
    rmdir "$ROOT" 2>/dev/null || true
    info "Removed $ROOT and $APPLICATIONS."
    [ -d "$PREFIX" ] || return 0
    if [ "$INTERACTIVE" = true ] && confirm "    Also delete Office and its settings in $PREFIX?" n; then
        rm -rf "$PREFIX"
        info "Removed $PREFIX."
    else
        info "Office and its settings are still in $PREFIX; delete that folder to remove them."
    fi
}

preflight
if [ "$UNINSTALL" = true ]; then
    uninstall
    exit 0
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/wine4office-install.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

printf '%sWine4Office for macOS%s (experimental)\n' "$BOLD" "$RESET"
info "Wine4Office goes to:  $ROOT"
[ "$NO_OFFICE" = true ] || info "Office goes to:       $PREFIX"
info "App launchers go to:  $APPLICATIONS"
confirm "    Continue?" || exit 0

if [ -z "$PACKAGE" ]; then
    download
fi
install_package

if [ "$NO_OFFICE" = false ]; then
    if office_installed; then
        info "Office is already installed in $PREFIX; keeping it."
    else
        choose_product
        choose_language
        check_space
    fi
fi
[ -n "$PRODUCT" ] || PRODUCT=$DEFAULT_PRODUCT
[ -n "$LANGUAGE" ] || LANGUAGE=en-us
setup
finish
