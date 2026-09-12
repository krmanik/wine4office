#!/usr/bin/env python3
"""Install and start Microsoft Office with Wine4Office on macOS.

The Wine4Office Manager GUI is Linux-only, so this drives the same backend
code (prefix creation, Office Deployment Tool install, app lookup) from the
command line against a locally built x86_64 runner. See MACOS.md.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

MANAGER = Path(__file__).resolve().parents[1] / "wine4office-manager"
GRAPHICS_POLICY = r"HKCU\Software\Policies\Microsoft\Office\16.0\Common\Graphics"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--runner", type=Path, required=True,
                        help="Wine4Office install prefix containing bin/wine")
    parser.add_argument("--deps", type=Path, required=True,
                        help="prefix of the x86_64 FreeType, GnuTLS and Kerberos builds")
    parser.add_argument("--moltenvk", type=Path, required=True,
                        help="directory containing libMoltenVK.dylib")
    parser.add_argument("--prefix", type=Path, default=Path.home() / ".wine4office-mac",
                        help="Wine prefix (default: %(default)s)")
    parser.add_argument("--product", default="O365ProPlusRetail",
                        help="Office product ID (default: %(default)s)")
    parser.add_argument("--language", default="en-us", help="Office language (default: %(default)s)")
    parser.add_argument("--log", type=Path, help="write the app's Wine output here (run only)")
    parser.add_argument("command", choices=("prefix", "office", "repair", "run"))
    parser.add_argument("app", nargs="?", default="word",
                        help="app to start with 'run': word, excel or powerpoint (default: %(default)s)")
    return parser.parse_args()


def output(line: str) -> None:
    print(line, flush=True)


def progress(message: str, percent) -> None:
    output(f"  {message}" + (f" ({percent}%)" if percent is not None else ""))


def main() -> int:
    args = parse_args()
    wine = args.runner / "bin" / "wine"
    if not wine.is_file():
        output(f"{wine} not found; build and install Wine4Office first (see MACOS.md).")
        return 1

    # Every Wine process the backend starts inherits this environment.
    os.environ.setdefault("SSL_CERT_FILE", "/etc/ssl/cert.pem")  # python.org Python has no CA bundle
    os.environ["DYLD_FALLBACK_LIBRARY_PATH"] = os.pathsep.join([str(args.deps / "lib"), str(args.moltenvk)])
    os.environ["WINE4OFFICE_VK_RELAXED_FL"] = "1"  # MoltenVK: allow Direct3D feature level 10_0+
    os.environ.setdefault("MVK_CONFIG_LOG_LEVEL", "0")

    sys.path.insert(0, str(MANAGER))
    import wine4office_backend as backend

    prefix = str(args.prefix)

    if args.command == "prefix":
        output(backend.create_environment(prefix, str(wine), False, output, progress_callback=progress))
        # The backend selects the X11/Wayland drivers; macOS uses its own.
        subprocess.run([str(wine), "reg", "add", r"HKCU\Software\Wine\Drivers", "/v", "Graphics",
                        "/d", "mac", "/f"], env=backend.wine_environment(prefix, wine), check=True)
        output("Graphics driver set to mac.")

    elif args.command in ("office", "repair"):
        payload = backend.build_office_configuration(args.product, [args.language]).encode()
        config = args.prefix / "wine4office-office-configuration.xml"
        config.write_bytes(payload)
        if args.command == "repair":
            output(backend.repair_office(prefix, str(wine), "quick", output))
        else:
            output(backend.install_office_with_odt(prefix, str(wine), str(config), output,
                                                   configuration_payload=payload))
        output(f"Word: {backend.find_office_app(prefix, 'word') or 'not found'}")

    elif args.command == "run":
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
        stream = args.log.open("w") if args.log else None
        process = subprocess.Popen([str(wine), str(app)], env=env, stdout=stream, stderr=subprocess.STDOUT)
        output(f"Started {args.app} (pid {process.pid})" + (f"; log: {args.log}" if args.log else ""))

    return 0


if __name__ == "__main__":
    sys.exit(main())
