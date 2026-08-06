#include "vial_controller.h"

#include "logger.h"

#include <chrono>
#include <thread>
#include <cstring>


VialController::VialController()
{
}


VialController::~VialController()
{
    shutdown();
}



bool VialController::initialize()
{
    Logger::info("Initializing VialController...");


    if (hid_init() != 0)
    {
        Logger::error("hid_init failed.");
        return false;
    }


    if (!discover())
    {
        Logger::error("No Vial keyboards discovered.");
        return false;
    }


    Logger::info(
        "VialController initialized with "
        + std::to_string(devices_.size())
        + " devices."
    );


    return true;
}



void VialController::shutdown()
{
    for (auto& device : devices_)
    {
        if (device.handle)
        {
            hid_close(device.handle);
            device.handle = nullptr;
        }
    }


    devices_.clear();


    hid_exit();


    Logger::info("VialController shut down.");
}



int VialController::deviceCount() const
{
    return static_cast<int>(devices_.size());
}



const std::string& VialController::devicePath(int device_index) const
{
    static const std::string empty;

    if (device_index < 0 || device_index >= deviceCount())
        return empty;

    return devices_[device_index].path;
}



bool VialController::discover()
{
    hid_device_info* list =
        hid_enumerate(
            TARGET_VID,
            TARGET_PID
        );


    if (!list)
        return false;



    for (
        hid_device_info* current = list;
        current;
        current = current->next
    )
    {

        if (
            current->usage_page != USAGE_PAGE ||
            current->usage != USAGE_ID
        )
        {
            continue;
        }



        hid_device* handle =
            hid_open_path(
                current->path
            );


        if (!handle)
            continue;



        Device device;

        device.handle = handle;
        device.path = current->path;
        device.connected = true;


        devices_.push_back(device);
    }



    hid_free_enumeration(list);


    return !devices_.empty();
}



bool VialController::sendCommand(
    hid_device* device,
    const std::vector<uint8_t>& payload,
    std::vector<uint8_t>& response
)
{
    uint8_t buffer[REPORT_LENGTH + 1] = {0};


    size_t length =
        payload.size();


    if (length > REPORT_LENGTH)
        return false;



    memcpy(
        buffer + 1,
        payload.data(),
        length
    );



    if (
        hid_write(
            device,
            buffer,
            sizeof(buffer)
        ) < 0
    )
    {
        return false;
    }



    uint8_t reply[REPORT_LENGTH] = {0};


    if (
        hid_read_timeout(
            device,
            reply,
            sizeof(reply),
            1000
        ) < 0
    )
    {
        return false;
    }



    response.assign(
        reply,
        reply + REPORT_LENGTH
    );


    return true;
}



bool VialController::setLightingMode(
    hid_device* device,
    uint8_t mode,
    const Color& color
)
{
    std::vector<uint8_t> response;


    std::vector<uint8_t> payload =
    {
        COMMAND_LIGHTING_SET,
        VIALRGB_MODE,
        mode,
        0,
        color.brightness,
        color.hue,
        color.saturation,
        0
    };



    return sendCommand(
        device,
        payload,
        response
    );
}



bool VialController::setColor(
    int device_index,
    const Color& color
)
{
    if (
        device_index < 0 ||
        device_index >= deviceCount()
    )
    {
        return false;
    }



    return setLightingMode(
        devices_[device_index].handle,
        2,
        color
    );
}



bool VialController::turnOff(
    int device_index
)
{
    if (
        device_index < 0 ||
        device_index >= deviceCount()
    )
    {
        return false;
    }



    Color black;

    black.hue = 0;
    black.saturation = 0;
    black.brightness = 0;



    return setLightingMode(
        devices_[device_index].handle,
        0,
        black
    );
}



bool VialController::flash(
    int device_index,
    const Color& color,
    int milliseconds
)
{
    if (!setColor(device_index, color))
        return false;



    std::this_thread::sleep_for(
        std::chrono::milliseconds(milliseconds)
    );


    return turnOff(device_index);
}



bool VialController::save(
    int device_index
)
{
    if (
        device_index < 0 ||
        device_index >= deviceCount()
    )
    {
        return false;
    }



    std::vector<uint8_t> response;


    return sendCommand(
        devices_[device_index].handle,
        {
            COMMAND_LIGHTING_SAVE
        },
        response
    );
}



bool VialController::configureKeyboardLayout(
    int device_index,
    uint8_t layer,
    uint8_t row,
    uint8_t column,
    uint16_t keycode
)
{
    if (
        device_index < 0 ||
        device_index >= deviceCount()
    )
    {
        return false;
    }



    uint8_t high =
        static_cast<uint8_t>(
            keycode >> 8
        );

    uint8_t low =
        static_cast<uint8_t>(
            keycode & 0xff
        );



    std::vector<uint8_t> response;



    return sendCommand(
        devices_[device_index].handle,
        {
            COMMAND_KEYMAP_SET,
            layer,
            row,
            column,
            high,
            low
        },
        response
    );
}
