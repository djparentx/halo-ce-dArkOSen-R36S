#!/usr/bin/env python3
"""The launcher's updater (port/knulli/README.md): before the game starts,
asks GitHub for this port's latest release and, when it is newer than the
installed one (the VERSION file a release puts beside the game), offers it
on the screen. START updates: the release's zip for this install (the
Knulli zip, or the PortMaster zip for PortMaster's launcher) is downloaded,
checked against the SHA-256 GitHub gives for it, and its files put in place
of the installed ones; maps/, save/, custom_maps/ and config.toml are never
touched. Any other button, or 30 seconds, plays the installed version, and
that release is not offered again (the next one is).

    halo_update.py <game folder> <launcher> <knulli|portmaster>

Exit status 10: updated (the launcher starts itself again); 11: an update
stopped part way and could not be finished (the files are a mix of two
versions: the launcher starts no game while update.journal is there,
whatever this exits with); anything else: go on to the game. Off with
update.auto = false in config.toml (Settings, Network Setup, CHECK FOR
UPDATES), for an installation without a VERSION file (a build of one's
own), with no internet connection, or with no screen or buttons to ask
with; the check gives up after a few seconds, and is made once in six hours
(save/update-checked) at most.

An install is a transaction: the zip is kept in update.partial/, every file
is written beside its place, then a journal (update.journal) names the
release and its zip, and only then are the files moved into place, VERSION
last. A start that finds the journal (the power went, the card was pulled,
the updater was stopped) installs the kept zip again before anything else,
or the latest release downloaded again; failing both, it stops, and the
journal stays. Python's standard library only.
"""

import glob
import hashlib
import json
import os
import re
import select
import shutil
import sys
import threading
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import halo_screen  # noqa: E402

REPOSITORY = "kirklandsig/halo-ce-anbernic-rg35xx"
LATEST = f"https://api.github.com/repos/{REPOSITORY}/releases/latest"
# the zips' own folders, by install kind: the launcher's name, the game's folder
LAYOUTS = {"knulli": ("roms/ports/Halo.sh", "roms/ports/halo/"), "portmaster": ("Halo.sh", "halo/")}
# never replaced, should a zip hold them (VERSION is written once all the
# rest is in place)
KEPT_FILES = ("config.toml", "VERSION")
KEPT_FOLDERS = ("maps", "save", "custom_maps")
LARGEST = 200 * 1024 * 1024
# the whole check, and the whole download, at most; a check once in this
# many seconds at most (the time of the last in CHECKED)
CHECK_SECONDS = 8
DOWNLOAD_SECONDS = 600
CHECK_EVERY = 6 * 3600
JOURNAL = "update.journal"
PARTIAL = "update.partial"
CHECKED = os.path.join("save", "update-checked")
UPDATED = 10
STOPPED = 11
# START's code on a standard pad; the H700 handhelds' is 311 (the kernel's
# BTN_TR), as EmulationStation's configuration says (start_codes)
BTN_START = 315
H700_START = 311
# (no input device can be read: nobody can answer)
NO_INPUT = -1
# what a check, a download or an install can fail with (the network's own
# errors come as OSError: within)
FAILURES = (OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile)


def log(text):
    try:
        print(f"update: {text}", flush=True)
    except OSError:
        pass


def say(screen, title, text, fraction=None):
    """on the screen, if there is one that works: a screen that fails changes
    nothing of what the updater does"""
    try:
        if screen:
            screen.show(title, text, fraction)
    except Exception as error:  # (the framebuffer's, whatever it is)
        log(f"the screen: {error}")


def version_numbers(tag):
    """v2026.10.15.1 as (2026, 10, 15, 1); None for a tag of another form
    (release_stage.py makes only this form)"""
    found = re.fullmatch(r"v(\d+(?:\.\d+)*)", tag.strip())
    return tuple(int(part) for part in found.group(1).split(".")) if found else None


def auto_update(game):
    """config.toml's update.auto (true when the file or the key is missing)"""
    section = None
    try:
        with open(os.path.join(game, "config.toml"), encoding="utf-8", errors="replace") as config:
            for line in config:
                line = line.split("#", 1)[0].strip()
                if line.startswith("["):
                    section = line.strip("[] ")
                elif section == "update" and "=" in line:
                    key, value = (part.strip() for part in line.split("=", 1))
                    if key == "auto":
                        return value.lower() != "false"
    except OSError:
        pass
    return True


def within(seconds, function, *arguments):
    """function's result, given up after the seconds: a slow answer, a name
    lookup that hangs or a read that a trickle keeps going, which no socket
    timeout bounds (the thread is left behind; the process ends soon after)"""
    outcome = {}

    def run():
        try:
            outcome["result"] = function(*arguments)
        except Exception as error:  # (handed to the caller)
            outcome["error"] = error

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    thread.join(seconds)
    if "error" in outcome:
        error = outcome["error"]
        # (http.client's own, a connection that broke, as an OSError)
        raise error if isinstance(error, FAILURES) else OSError(f"{type(error).__name__}: {error}")
    if "result" not in outcome:
        raise TimeoutError(f"not done in {seconds} s")
    return outcome["result"]


def checked_recently(game):
    """a check within CHECK_EVERY seconds, else the time of this one noted
    (either way: the handheld's clock can go back as well as on)"""
    path = os.path.join(game, CHECKED)
    now = time.time()
    try:
        with open(path, encoding="utf-8") as file:
            if abs(now - float(file.read())) < CHECK_EVERY:
                return True
    except (OSError, ValueError):
        pass
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as file:
            file.write(f"{now:.0f}\n")
    except OSError:
        pass
    return False


def request(url, timeout):
    # (imported here: most starts check nothing, and it takes a while)
    import urllib.request
    return urllib.request.urlopen(urllib.request.Request(url, headers={
        "User-Agent": "halo-ce-anbernic-rg35xx-updater", "Accept": "application/vnd.github+json"}), timeout=timeout)


def latest_release(kind):
    """(tag, the zip's address, its size, its SHA-256) of the latest release"""
    with request(LATEST, 5) as answer:
        release = json.load(answer)
    tag = release["tag_name"]
    name = f"halo-ce-{kind}-{tag}.zip"
    for asset in release.get("assets", []):
        if asset.get("name") == name:
            digest = asset.get("digest") or ""
            if not digest.startswith("sha256:"):
                raise ValueError(f"{name} has no SHA-256 to check it by")
            return tag, asset["browser_download_url"], int(asset["size"]), digest[7:].lower()
    raise ValueError(f"the release {tag} has no {name}")


def wait_for_key(seconds):
    """the code of the first key pressed (not one held from the start), None
    once the seconds pass, or NO_INPUT when no input device can be read"""
    devices = []
    for path in glob.glob("/dev/input/event*"):
        try:
            devices.append(os.open(path, os.O_RDONLY | os.O_NONBLOCK))
        except OSError:
            pass
    now = time.monotonic()
    deadline, ignore_until = now + seconds, now + 0.5
    try:
        while devices:
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            ready, _, _ = select.select(devices, [], [], left)
            for device in ready:
                try:
                    data = os.read(device, halo_screen.EVENT.size * 64)
                except OSError:
                    data = b""
                if not data:
                    devices.remove(device)
                    os.close(device)
                    continue
                for _, _, kind, code, value in halo_screen.EVENT.iter_unpack(
                        data[:len(data) - len(data) % halo_screen.EVENT.size]):
                    if kind == halo_screen.EV_KEY and value == 1 and time.monotonic() >= ignore_until:
                        return code
        # (none, or every one gone)
        return NO_INPUT
    finally:
        for device in devices:
            os.close(device)


def start_codes():
    """the codes START sends: the standard one and the H700 handhelds' (also
    with another pad connected, or no configuration: muOS), and those
    EmulationStation's configuration of a connected pad names start (as
    sdl_mapping.py reads it)"""
    import xml.etree.ElementTree as ElementTree
    import sdl_mapping
    codes = {BTN_START, H700_START}
    names = sdl_mapping.connected_names()
    for path in sdl_mapping.CONFIGS:
        try:
            root = ElementTree.parse(path).getroot()
        except (OSError, ValueError, LookupError, ElementTree.ParseError):
            continue
        for config in root.iter("inputConfig"):
            if config.get("type") != "joystick" or config.get("deviceName") not in names:
                continue
            for entry in config.iter("input"):
                if entry.get("name") == "start" and entry.get("type") == "button":
                    try:
                        codes.add(int(entry.get("code")))
                    except (TypeError, ValueError):
                        pass
    return codes


def sha256_of(path):
    hasher = hashlib.sha256()
    with open(path, "rb") as file:
        for block in iter(lambda: file.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def sync_file(file):
    file.flush()
    os.fsync(file.fileno())


def sync_folder(path):
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def staging(path, mode=0o644):
    """path.update, new (a link left there is not written through), open for
    writing, with its mode"""
    new = path + ".update"
    if os.path.lexists(new):
        os.remove(new)
    descriptor = os.open(new, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    os.fchmod(descriptor, mode)
    return new, os.fdopen(descriptor, "wb")


def write_durably(path, text):
    """path's whole text, through path.update: the old one or the new one,
    whatever stops it"""
    new, file = staging(path)
    with file:
        file.write(text.encode("utf-8"))
        sync_file(file)
    os.replace(new, path)
    sync_folder(os.path.dirname(path))


def download(url, size, digest, path, screen, tag):
    """the zip to path, checked, and its folder synced; the progress on the
    screen"""
    hasher = hashlib.sha256()
    done = shown = 0
    with request(url, 20) as answer, open(path, "wb") as out:
        while True:
            block = answer.read(64 * 1024)
            if not block:
                break
            done += len(block)
            if done > LARGEST:
                raise ValueError("the download is larger than any release")
            hasher.update(block)
            out.write(block)
            # (the bar drawn again only for a whole percent more)
            percent = done * 100 // size if size else 0
            try:
                if screen and percent != shown:
                    shown = percent
                    screen.progress(percent / 100)
            except Exception:  # (as say's)
                screen = None
        sync_file(out)
    sync_folder(os.path.dirname(path))
    if done != size or hasher.hexdigest() != digest:
        raise ValueError(f"the download of {tag} is not the release's file (size {done} of {size}, or its SHA-256)")


def fetch(kind, game, screen, release=None):
    """the release's zip (the latest's, when none is given), downloaded into
    update.partial/: (its tag, the zip, its SHA-256)"""
    tag, url, size, digest = release or within(CHECK_SECONDS, latest_release, kind)
    partial = os.path.join(game, PARTIAL)
    archive = os.path.join(partial, f"halo-ce-{kind}-{tag}.zip")
    shutil.rmtree(partial, ignore_errors=True)
    os.makedirs(partial)
    say(screen, f"Downloading {tag}", "From github.com/" + REPOSITORY, 0.0)
    within(DOWNLOAD_SECONDS, download, url, size, digest, archive, screen, tag)
    return tag, archive, digest


def zip_target(name, kind, game, launcher):
    """where a member of the zip goes, None for one kept, or ValueError for
    one no release holds (a name not in its plain form, or outside its
    folders)"""
    launcher_name, folder = LAYOUTS[kind]
    if name == launcher_name:
        return launcher
    if not name.startswith(folder):
        raise ValueError(f"a file the zip should not hold: {name}")
    parts = name[len(folder):].split("/")
    if any(part in ("", ".", "..") or "\\" in part for part in parts):
        raise ValueError(f"a name the zip should not hold: {name}")
    if (len(parts) == 1 and parts[0] in KEPT_FILES) or parts[0] in KEPT_FOLDERS:
        return None
    # (no folder on the way that is a link elsewhere)
    path = game
    for part in parts[:-1]:
        path = os.path.join(path, part)
        if os.path.islink(path):
            raise ValueError(f"{path} is a link: not written through")
    return os.path.join(game, *parts)


def install(archive, kind, game, launcher, journal, journal_text):
    """the zip's files in place of the installed ones: each written beside
    its place first, then the journal, then all moved in"""
    staged = []
    try:
        with zipfile.ZipFile(archive) as zip_file:
            for member in zip_file.infolist():
                if member.is_dir():
                    continue
                target = zip_target(member.filename, kind, game, launcher)
                if target is None:
                    continue
                os.makedirs(os.path.dirname(target), exist_ok=True)
                mode = (member.external_attr >> 16) & 0o777
                new, out = staging(target, mode or (0o755 if target.endswith((".sh", "/halo")) else 0o644))
                staged.append((new, target))
                with zip_file.open(member) as source, out:
                    shutil.copyfileobj(source, out)
                    sync_file(out)
        folders = {os.path.dirname(target) for _, target in staged} | {game}
        for folder in folders:
            sync_folder(folder)
        write_durably(journal, journal_text)
        for new, target in staged:
            os.replace(new, target)
        staged = []
        for folder in folders:
            sync_folder(folder)
    finally:
        for new, _ in staged:
            try:
                os.remove(new)
            except OSError:
                pass


def finish(archive, kind, game, launcher, tag, digest):
    """the release installed from its zip, VERSION written last and the
    journal and the zip let go"""
    journal = os.path.join(game, JOURNAL)
    install(archive, kind, game, launcher, journal,
            json.dumps({"tag": tag, "digest": digest, "archive": os.path.basename(archive)}) + "\n")
    write_durably(os.path.join(game, "VERSION"), tag + "\n")
    os.remove(journal)
    sync_folder(game)
    shutil.rmtree(os.path.join(game, PARTIAL), ignore_errors=True)


def stopped(screen, error):
    """an install that moved some files in and could not finish: the journal
    stays and the launcher stops (the next start tries again)"""
    log(f"stopped part way: {error}")
    say(screen, "Update not finished", f"{error}\n\nThe update stopped part way, so Halo can't start. "
        "Connect to the internet (or free some space on the SD card) and start Halo again, which "
        "finishes it; or unzip the latest release over the files. Press a button.")
    try:
        if screen:
            wait_for_key(30)
    except Exception:  # (nothing changes the answer)
        pass
    return STOPPED


def resume(game, launcher, kind):
    """an install a start found unfinished (its journal): installed again from
    the zip kept for it, else from the latest release downloaded again;
    None when there is none to resume"""
    journal = os.path.join(game, JOURNAL)
    if not os.path.lexists(journal):
        return None
    screen = None
    try:
        screen = halo_screen.open_screen()
    except Exception as error:  # (as say's)
        log(f"the screen: {error}")
    say(screen, "Finishing the update", "The last update stopped part way: putting its files in place.")
    try:
        with open(journal, encoding="utf-8") as file:
            entry = json.load(file)
        tag, digest = entry["tag"], entry["digest"]
        archive = os.path.join(game, PARTIAL, os.path.basename(entry["archive"]))
        if sha256_of(archive) != digest:
            raise ValueError("the zip kept is not the release's file")
        finish(archive, kind, game, launcher, tag, digest)
        log(f"updated to {tag} (finished)")
        return UPDATED
    except FAILURES as error:
        log(f"the kept update cannot be finished ({error}): downloading the latest release")
    try:
        tag, archive, digest = fetch(kind, game, screen)
        say(screen, f"Installing {tag}", "Putting the new files in place.")
        finish(archive, kind, game, launcher, tag, digest)
    except FAILURES as error:
        return stopped(screen, error)
    log(f"updated to {tag} (downloaded again)")
    return UPDATED


def main():
    if len(sys.argv) != 4 or sys.argv[3] not in LAYOUTS:
        raise SystemExit(f"usage: {sys.argv[0]} <game folder> <launcher> <knulli|portmaster>")
    game, launcher, kind = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2]), sys.argv[3]
    resumed = resume(game, launcher, kind)
    if resumed is not None:
        return resumed
    try:
        with open(os.path.join(game, "VERSION"), encoding="utf-8") as file:
            installed = file.read().strip()
    except OSError:
        return 0
    current = version_numbers(installed)
    if not current or not auto_update(game) or checked_recently(game):
        return 0
    try:
        release = within(CHECK_SECONDS, latest_release, kind)
    except FAILURES as error:
        log(f"no answer from GitHub ({error}); playing {installed}")
        return 0
    tag, _, size, _ = release
    declined = os.path.join(game, "save", "update-declined")
    try:
        with open(declined, encoding="utf-8") as file:
            if file.read().strip() == tag:
                return 0
    except OSError:
        pass
    newest = version_numbers(tag)
    if not newest or newest <= current:
        return 0
    log(f"{tag} is out (installed: {installed})")
    screen = halo_screen.open_screen()
    if not screen:
        log("no screen to ask on; playing")
        return 0
    say(screen, "Update available", f"Halo {tag} is out (this is {installed}).\n\n"
        f"Press START to update now: about {size // (1024 * 1024) + 1} MB, and your maps, saves "
        "and settings stay.\n\nPress any other button to play this version (it is not offered "
        "again; turn checking off in Settings, Network Setup, CHECK FOR UPDATES).")
    key = wait_for_key(30)
    if key == NO_INPUT:
        log("no buttons to answer with; playing")
        return 0
    if key not in start_codes():
        log(f"{tag} not taken now")
        try:
            os.makedirs(os.path.dirname(declined), exist_ok=True)
            with open(declined, "w", encoding="utf-8") as file:
                file.write(tag + "\n")
        except OSError:
            pass
        return 0
    try:
        tag, archive, digest = fetch(kind, game, screen, release)
        say(screen, f"Installing {tag}", "Putting the new files in place.")
        finish(archive, kind, game, launcher, tag, digest)
    except FAILURES as error:
        if os.path.lexists(os.path.join(game, JOURNAL)):
            return stopped(screen, error)
        shutil.rmtree(os.path.join(game, PARTIAL), ignore_errors=True)
        log(f"failed: {error}")
        say(screen, "Update failed", f"{error}\n\nThe installed version is unchanged. Press a button to play it.")
        wait_for_key(30)
        return 0
    log(f"updated to {tag}")
    say(screen, "Updated", f"Halo {tag} is installed. Starting it.")
    time.sleep(1.5)
    return UPDATED


if __name__ == "__main__":
    sys.exit(main())
