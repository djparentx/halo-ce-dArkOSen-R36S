#!/usr/bin/env python3
"""Prints SDL_GAMECONTROLLERCONFIG lines for the handheld's own controls,
from EmulationStation's controller configuration (port/knulli/README.md).

SDL2's built-in database takes the H700 handhelds' controls (all of them
report the same generic GUID) for an ODROID-GO 2 pad, with the wrong
buttons. EmulationStation knows each device's controls by name; this turns
the entry of every connected device into an SDL mapping, as Batocera's
emulator launcher does: EmulationStation's buttons are named by the
Nintendo layout (a on the right, b at the bottom), SDL's by the Xbox layout
(a at the bottom).
"""

import re
import xml.etree.ElementTree as ElementTree

CONFIGS = [
    "/etc/emulationstation/es_input.cfg",
    "/home/ark/.emulationstation/es_input.cfg",
    "/userdata/system/configs/emulationstation/es_input.cfg",
    "/usr/share/emulationstation/es_input.cfg",
]
# EmulationStation's name -> SDL's
NAMES = {
    "b": "a", "a": "b", "y": "x", "x": "y",
    "select": "back", "start": "start", "hotkey": "guide",
    "pageup": "leftshoulder", "pagedown": "rightshoulder",
    "l2": "lefttrigger", "r2": "righttrigger", "l3": "leftstick", "r3": "rightstick",
    "up": "dpup", "down": "dpdown", "left": "dpleft", "right": "dpright",
    "joystick1left": "leftx", "joystick1up": "lefty", "joystick2left": "rightx", "joystick2up": "righty",
    "leftanalogleft": "leftx", "leftanalogup": "lefty", "rightanalogleft": "rightx", "rightanalogup": "righty",
    "leftthumb": "leftstick", "rightthumb": "rightstick", "lefttrigger": "lefttrigger", "righttrigger": "righttrigger",
}


def connected_names():
    names = set()
    try:
        with open("/proc/bus/input/devices", encoding="utf-8", errors="replace") as devices:
            for line in devices:
                found = re.match(r'N: Name="(.*)"', line)
                if found:
                    names.add(found.group(1))
    except OSError:
        pass
    return names


# the sticks' axes, named in EmulationStation by the direction that gives
# the value it notes (left and up are negative in SDL)
STICKS = {"leftx", "lefty", "rightx", "righty"}
# the triggers' axes: their whole travel (SDL reads an axis end to end, and a
# trigger rests at the low end), the other way round when pressing one lowers
# it
TRIGGERS = {"lefttrigger", "righttrigger"}


def element(entry, sdl_name):
    kind, identifier, value = entry.get("type"), entry.get("id"), entry.get("value")
    if kind == "button":
        return f"b{identifier}"
    if kind == "hat":
        return f"h{identifier}.{value}"
    if kind == "axis":
        try:
            negative = int(value) < 0
        except (TypeError, ValueError):
            return None
        if sdl_name in STICKS:
            # a stick whose left or up is positive is the other way round
            return f"a{identifier}" if negative else f"a{identifier}~"
        if sdl_name in TRIGGERS:
            return f"a{identifier}~" if negative else f"a{identifier}"
        # a button or a direction of the pad on half an axis
        return f"-a{identifier}" if negative else f"+a{identifier}"
    return None


def main():
    names = connected_names()
    done = set()
    for path in CONFIGS:
        try:
            root = ElementTree.parse(path).getroot()
        except (OSError, ElementTree.ParseError):
            continue
        for config in root.iter("inputConfig"):
            name, guid = config.get("deviceName"), config.get("deviceGUID")
            if config.get("type") != "joystick" or name not in names or not guid or guid in done:
                continue
            parts = [guid, name]
            for entry in config.iter("input"):
                sdl_name = NAMES.get(entry.get("name"))
                value = element(entry, sdl_name)
                if sdl_name and value:
                    parts.append(f"{sdl_name}:{value}")
            parts.append("platform:Linux")
            print(",".join(parts) + ",")
            done.add(guid)


if __name__ == "__main__":
    main()
