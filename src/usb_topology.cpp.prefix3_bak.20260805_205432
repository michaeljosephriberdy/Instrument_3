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

    std::string resolved_str = resolved.string();

    // Matches a pure USB-device-level path segment like "1-1.4" but
    // NOT an interface-level segment like "1-1.4:1.1" (which has a
    // colon). Walking all matches and keeping the last one gives us
    // the deepest (most specific) device-level segment, which is what
    // both interfaces of the same physical device share.
    static const std::regex usb_dev_re(R"(/([0-9]+-[0-9.]+)(/|$))");

    std::string best;

    auto begin = std::sregex_iterator(resolved_str.begin(), resolved_str.end(), usb_dev_re);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it)
    {
        best = (*it)[1].str();
    }

    return best;
}
