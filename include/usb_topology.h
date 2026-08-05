#pragma once

#include <string>

// Resolves the USB *device* port address (e.g. "1-1.4") for a given
// /dev/input/eventN or /dev/hidrawN node, by walking its sysfs class
// symlink.
//
// Our ID75 keyboards expose two USB interfaces: a boot-keyboard HID
// interface (consumed via evdev, e.g. /dev/input/event7) and a
// vendor-defined raw-HID interface for Vial/RGB (consumed via hidraw,
// e.g. /dev/hidraw3). Both interfaces hang off the same physical USB
// device, so their sysfs paths share a device-level segment like
// ".../1-1.4/1-1.4:1.0/..." vs ".../1-1.4/1-1.4:1.1/...". This function
// returns that shared "1-1.4" segment, so two device nodes belonging
// to the same physical keyboard resolve to equal strings.
//
// All four keyboards report the same USB VID/PID/name, so this port
// address is the ONLY reliable way to tell them apart.
//
// Returns an empty string if the node can't be resolved (unplugged
// mid-lookup, unexpected kernel layout, etc.) -- callers should treat
// that as "couldn't correlate," not as a hard error.
std::string usbDevicePortAddress(const std::string& dev_node_path);
