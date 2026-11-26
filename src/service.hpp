#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace tileflow {

struct Config {
    std::filesystem::path executable;
    std::filesystem::path web;
    std::string disk = "disk.img";
    unsigned disk_mib = 256;
    unsigned port = 8080;
    unsigned workers = 4;
    int tile_size = 256;
    int delay_ms = 0;
    bool fifo = false;
};

int serve(const Config& options);

}
