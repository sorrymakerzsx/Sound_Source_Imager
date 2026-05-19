#include <cstdlib>
#include <iostream>

#include "backlight_pwm.h"
#include "eeprom_version.h"

int main(int argc, char **argv) {
    BacklightDeviceInfo bl;
    if (probe_backlight_device(&bl)) {
        std::cout << "Backlight: " << bl.name << " brightness="
                  << bl.brightness << "/" << bl.max_brightness
                  << " path=" << bl.base_path << std::endl;
        if (argc == 3 && std::string(argv[1]) == "--set-bl") {
            int target = std::atoi(argv[2]);
            if (!set_backlight_brightness(bl.base_path, target)) {
                std::cerr << "Failed to set backlight brightness." << std::endl;
                return 2;
            }
            BacklightDeviceInfo updated;
            probe_backlight_device(&updated);
            std::cout << "Backlight updated: " << updated.brightness << "/"
                      << updated.max_brightness << std::endl;
        }
    } else {
        std::cout << "Backlight: not found" << std::endl;
    }

    EepromVersionInfo eeprom;
    if (read_first_available_eeprom(&eeprom)) {
        std::cout << "EEPROM: " << eeprom.device_path << " text="
                  << (eeprom.text.empty() ? "<non-printable>" : eeprom.text) << std::endl;
        std::cout << "EEPROM raw: " << format_eeprom_hex(eeprom.raw_bytes) << std::endl;
    } else {
        std::cout << "EEPROM: not found" << std::endl;
    }

    return 0;
}
