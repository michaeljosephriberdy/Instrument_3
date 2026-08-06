#include "usb_topology.h"

#include <filesystem>
#include <regex>

namespace
{
    std::string basenameOf(const std::string& path)
    {
        auto pos = path.find_last_of('/');
        return (pos == std::string::npos) ? path : path.substr(pos + 1);
    }
}

std::string usbDevicePortAddress(const std::string& dev_node_path)
{
    std::string name = basenameOf(dev_node_path);
    std::string sysfs_class_path;
    if (name.rfind("event", 0) == 0)
    {
        sysfs_class_path = "/sys/class/input/" + name;
    }
    else if (name.rfind("hidraw", 0) == 0)
    {
        sysfs_class_path = "/sys/class/hidraw/" + name;
    }
    else
    {
        return "";
    }
    std::error_code ec;
    std::filesystem::path resolved =
        std::filesystem::canonical(sysfs_class_path, ec);
    if (ec)
        return "";
    // Walk up from the class device's resolved sysfs path until we find
    // the directory that represents the actual USB *device* (as opposed
    // to a USB *interface*, which is one level below it). The device
    // directory is the one and only ancestor containing an "idVendor"
    // file -- interface directories (named like "1-1.4:1.1") do not have
    // one. This is what two sibling interfaces of the same physical
    // composite device (e.g. the keyboard HID interface and the Vial
    // raw-HID interface) share, regardless of how deep the USB hub
    // chain is or how the port numbering looks.
    std::filesystem::path dir = resolved;
    for (int hops = 0; hops < 16 && dir.has_parent_path(); ++hops)
    {
        dir = dir.parent_path();
        std::error_code ec2;
        if (std::filesystem::exists(dir / "idVendor", ec2) && !ec2)
        {
            return dir.filename().string();
        }
    }
    return "";
}

    
