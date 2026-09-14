#!/usr/bin/env python3
"""Install and start Microsoft Office with Wine4Office on macOS.

The Wine4Office Manager GUI is Linux-only, so this drives the same backend
code (prefix creation, Office Deployment Tool install, app lookup) from the
command line. It runs from the prebuilt package that tools/macos/install.sh
installs, or from a source checkout against a locally built runner. See
MACOS.md.
"""
import argparse
import os
import plistlib
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
MANAGER = HERE.parent / "wine4office-manager"
# The prebuilt package keeps this script in tools/macos, beside runner/, deps/ and bin/.
PACKAGE = HERE.parents[1]
PACKAGED = (PACKAGE / "runner" / "bin" / "wine").is_file()
GRAPHICS_POLICY = r"HKCU\Software\Policies\Microsoft\Office\16.0\Common\Graphics"
APPLICATIONS = Path.home() / "Applications" / "Wine4Office"
LOGS = Path.home() / "Library" / "Logs" / "Wine4Office"
BUNDLE_ID = "io.github.wine4office.mac"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--runner", type=Path, default=PACKAGE / "runner" if PACKAGED else None,
                        help="Wine4Office install prefix containing bin/wine"
                             + (" (default: %(default)s)" if PACKAGED else ""))
    parser.add_argument("--deps", type=Path, default=PACKAGE / "deps" if PACKAGED else None,
                        help="prefix of the x86_64 FreeType, GnuTLS and Kerberos builds"
                             + (" (default: %(default)s)" if PACKAGED else ""))
    parser.add_argument("--moltenvk", type=Path, default=PACKAGE / "deps" / "lib" if PACKAGED else None,
                        help="directory containing libMoltenVK.dylib"
                             + (" (default: %(default)s)" if PACKAGED else ""))
    parser.add_argument("--prefix", type=Path, default=Path.home() / ".wine4office-mac",
                        help="Wine prefix (default: %(default)s)")
    parser.add_argument("--product", default="O365ProPlusRetail",
                        help="Office product ID; 'products' lists them (default: %(default)s)")
    parser.add_argument("--language", default="en-us",
                        help="Office language, or several separated by commas; 'languages' lists "
                             "them (default: %(default)s)")
    parser.add_argument("--no-office", action="store_true", help="'setup' only creates the prefix")
    parser.add_argument("--log", type=Path, help="write the app's Wine output here (run only)")
    parser.add_argument("command", choices=(
        "setup", "prefix", "office", "repair", "run", "shortcuts", "status", "products", "languages",
    ), help="setup: create the prefix, install Office if missing and add app launchers; "
            "prefix, office, shortcuts: one of those steps; repair: quick-repair Office; "
            "run: start an app; status: show what is installed; "
            "products, languages: list the Office editions and languages")
    parser.add_argument("app", nargs="?", default="word",
                        help="app to start with 'run': word, excel, powerpoint, ... (default: %(default)s)")
    return parser.parse_args()


def output(line: str) -> None:
    print(line, flush=True)


def progress(message: str, percent) -> None:
    output(f"  {message}" + (f" ({percent}%)" if percent is not None else ""))


def create_prefix(args, backend, wine: Path) -> None:
    prefix = str(args.prefix)
    output(backend.create_environment(prefix, str(wine), False, output, progress_callback=progress))
    # The backend selects the X11/Wayland drivers; macOS uses its own.
    subprocess.run([str(wine), "reg", "add", r"HKCU\Software\Wine\Drivers", "/v", "Graphics",
                    "/d", "mac", "/f"], env=backend.wine_environment(prefix, wine), check=True)
    output("Graphics driver set to mac.")


def install_office(args, backend, wine: Path, payload: bytes) -> None:
    config = args.prefix / "wine4office-office-configuration.xml"
    config.write_bytes(payload)
    output(backend.install_office_with_odt(str(args.prefix), str(wine), str(config), output,
                                           configuration_payload=payload))
    output(f"Word: {backend.find_office_app(str(args.prefix), 'word') or 'not found'}")


def installed_apps(args, backend) -> dict:
    """Map each installed Office app to its executable."""
    if not (args.prefix / "system.reg").is_file():
        return {}
    apps = {}
    for app in backend.APP_META:
        executable = backend.find_office_app(str(args.prefix), app)
        if executable is not None:
            apps[app] = executable
    return apps


def launch_command(args, app: str) -> list:
    if PACKAGED:
        command = [str(PACKAGE / "bin" / "wine4office")]
    else:
        command = [sys.executable, str(Path(__file__).resolve()), "--runner", str(args.runner),
                   "--deps", str(args.deps), "--moltenvk", str(args.moltenvk)]
    return command + ["--prefix", str(args.prefix), "run", app, "--log", str(LOGS / f"{app}.log")]


def largest_ico_png(data: bytes):
    """Return the largest PNG image in an .ico file, or None."""
    _, _, count = struct.unpack_from("<HHH", data)
    best, best_size = None, 0
    for index in range(count):
        width, height, _, _, _, _, size, offset = struct.unpack_from("<BBBBHHII", data, 6 + 16 * index)
        image = data[offset:offset + size]
        pixels = (width or 256) * (height or 256)
        if image.startswith(b"\x89PNG") and pixels > best_size:
            best, best_size = image, pixels
    return best


def make_icon(executable: Path, destination: Path, backend) -> bool:
    """Convert the app's Windows icon to .icns; return whether that worked."""
    with tempfile.TemporaryDirectory(prefix="wine4office-icon-") as temporary:
        work = Path(temporary)
        try:
            ico = backend.extract_office_icon(executable, work / "icon.ico")
            png = work / "icon.png"
            image = largest_ico_png(ico.read_bytes())
            if image is not None:
                png.write_bytes(image)
                source = png
            else:
                source = ico  # ImageIO reads .ico files too
            for command in (["sips", "-s", "format", "png", "-z", "256", "256", str(source), "--out", str(png)],
                            ["sips", "-s", "format", "icns", str(png), "--out", str(destination)]):
                subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return destination.is_file()
        except (OSError, RuntimeError, ValueError, struct.error, subprocess.CalledProcessError):
            return False


def write_app_bundle(bundle: Path, app: str, name: str, command: list, executable: Path, backend) -> None:
    shutil.rmtree(bundle, ignore_errors=True)
    contents = bundle / "Contents"
    (contents / "MacOS").mkdir(parents=True)
    (contents / "Resources").mkdir()
    launcher = contents / "MacOS" / "launch"
    launcher.write_text("#!/bin/sh\nexec " + shlex.join(command) + "\n")
    launcher.chmod(0o755)
    info = {
        "CFBundleName": name,
        "CFBundleDisplayName": name,
        "CFBundleIdentifier": f"{BUNDLE_ID}.{app}",
        "CFBundleExecutable": "launch",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": "1.0",
        "CFBundleVersion": "1",
        "LSMinimumSystemVersion": "11.0",
        # The launcher exits once Wine starts the app, and Wine gives the app its own Dock icon.
        "LSUIElement": True,
    }
    if make_icon(executable, contents / "Resources" / "AppIcon.icns", backend):
        info["CFBundleIconFile"] = "AppIcon"
    with open(contents / "Info.plist", "wb") as stream:
        plistlib.dump(info, stream)


def is_our_bundle(bundle: Path) -> bool:
    try:
        with open(bundle / "Contents" / "Info.plist", "rb") as stream:
            return plistlib.load(stream).get("CFBundleIdentifier", "").startswith(BUNDLE_ID + ".")
    except (OSError, plistlib.InvalidFileException):
        return False


def make_shortcuts(args, backend) -> None:
    """Add a macOS app to ~/Applications/Wine4Office for each installed Office app."""
    apps = installed_apps(args, backend)
    created = []
    for app, meta in backend.APP_META.items():
        bundle = APPLICATIONS / f"{meta['name']}.app"
        # Teams needs its own setup, and the Linux manager marks some apps as not working.
        if app in apps and app != "teams" and meta.get("compatibility") != "Not working":
            APPLICATIONS.mkdir(parents=True, exist_ok=True)
            write_app_bundle(bundle, app, meta["name"], launch_command(args, app), apps[app], backend)
            created.append(meta["name"])
        elif bundle.is_dir() and is_our_bundle(bundle):
            shutil.rmtree(bundle)
    if created:
        output(f"App launchers in {APPLICATIONS}: {', '.join(created)}")
    else:
        output("No Office apps are installed yet, so no app launchers were added.")


def setup(args, backend, wine: Path) -> int:
    # Check the edition and language before the long steps.
    payload = None if args.no_office else backend.build_office_configuration(args.product, args.language).encode()
    if (args.prefix / "system.reg").is_file():
        output(f"Wine prefix already exists: {args.prefix}")
    else:
        create_prefix(args, backend, wine)
    if payload is not None:
        if backend.office_installation_exists(str(args.prefix)):
            output("Office is already installed.")
        else:
            install_office(args, backend, wine, payload)
    make_shortcuts(args, backend)
    return 0


def run(args, backend, wine: Path) -> int:
    prefix = str(args.prefix)
    app = backend.find_office_app(prefix, args.app)
    if app is None:
        output(f"{args.app} is not installed in {prefix}.")
        return 1
    # Direct2D needs the Vulkan renderer on macOS.
    env = backend.wine_environment(prefix, wine, use_vulkan=True)
    # With hardware acceleration off, Office composites in software and draws nothing on Wine.
    subprocess.run([str(wine), "reg", "add", GRAPHICS_POLICY, "/v", "DisableHardwareAcceleration",
                    "/t", "REG_DWORD", "/d", "0", "/f"], env=env, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    stream = None
    if args.log:
        args.log.parent.mkdir(parents=True, exist_ok=True)
        stream = args.log.open("w")
    process = subprocess.Popen([str(wine), str(app)], env=env, stdout=stream, stderr=subprocess.STDOUT,
                               start_new_session=True)
    output(f"Started {args.app} (pid {process.pid})" + (f"; log: {args.log}" if args.log else ""))
    return 0


def status(args, backend) -> int:
    if PACKAGED and (PACKAGE / "VERSION").is_file():
        output(f"Wine4Office: {(PACKAGE / 'VERSION').read_text().strip()} in {PACKAGE}")
    ready = (args.prefix / "system.reg").is_file()
    output(f"Wine prefix: {args.prefix} ({'ready' if ready else 'not created'})")
    apps = [app for app in installed_apps(args, backend) if app != "teams"]
    output(f"Office apps: {', '.join(apps) or 'none'}")
    return 0 if apps else 1


def main() -> int:
    args = parse_args()
    sys.path.insert(0, str(MANAGER))
    import wine4office_backend as backend

    if args.command == "products":
        for product in backend.OFFICE_PRODUCTS:
            print(f"{product['product_id']}\t{product['label']}")
        return 0
    if args.command == "languages":
        print("\n".join(sorted(backend._OFFICE_LANGUAGE_IDS)))
        return 0
    if args.command == "status":
        return status(args, backend)

    if args.runner is None or args.deps is None or args.moltenvk is None:
        output("Pass --runner, --deps and --moltenvk for a locally built Wine4Office (see MACOS.md).")
        return 2
    wine = args.runner / "bin" / "wine"
    if not wine.is_file():
        output(f"{wine} not found; build and install Wine4Office first (see MACOS.md).")
        return 1

    # Every Wine process the backend starts inherits this environment.
    os.environ.setdefault("SSL_CERT_FILE", "/etc/ssl/cert.pem")  # standalone Python has no CA bundle
    libraries = dict.fromkeys([str(args.deps / "lib"), str(args.moltenvk)])
    os.environ["DYLD_FALLBACK_LIBRARY_PATH"] = os.pathsep.join(libraries)
    os.environ["WINE4OFFICE_VK_RELAXED_FL"] = "1"  # MoltenVK: allow Direct3D feature level 10_0+
    os.environ.setdefault("MVK_CONFIG_LOG_LEVEL", "0")

    try:
        if args.command == "setup":
            return setup(args, backend, wine)
        if args.command == "prefix":
            create_prefix(args, backend, wine)
        elif args.command == "office":
            install_office(args, backend, wine,
                           backend.build_office_configuration(args.product, args.language).encode())
        elif args.command == "repair":
            output(backend.repair_office(str(args.prefix), str(wine), "quick", output))
        elif args.command == "shortcuts":
            make_shortcuts(args, backend)
        elif args.command == "run":
            return run(args, backend, wine)
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        output(f"Error: {error}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
