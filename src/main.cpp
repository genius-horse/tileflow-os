#include "service.hpp"
#include "tileflow/worker.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

using namespace tileflow;

Bytes load(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() <= 0 || input.tellg() > 64 * 1024 * 1024)
        throw std::runtime_error("Cannot read input file or file exceeds 64 MiB: " + path);
    Bytes data(static_cast<std::size_t>(input.tellg()));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(data.data()), data.size()))
        throw std::runtime_error("Input read failed: " + path);
    return data;
}

void save(const std::string& path, const Bytes& data) {
    std::ofstream output(path, std::ios::binary);
    if (!output.write(reinterpret_cast<const char*>(data.data()), data.size()))
        throw std::runtime_error("Output write failed: " + path);
}

int number(const std::string& value, int lower, int upper) {
    std::size_t end = 0;
    const int n = std::stoi(value, &end);
    if (end != value.size() || n < lower || n > upper)
        throw std::invalid_argument("Argument outside supported range: " + value);
    return n;
}

}

int main(int argc, char** argv) {
    using namespace tileflow;
    try {
        const std::string action = argc >= 2 ? argv[1] : "";
        if (action == "--worker" && argc == 3)
            return worker_main(number(argv[2], 0, 1000000));
        if (action == "--serial" && argc == 5) {
            const Image source = decode(load(argv[3]));
            save(argv[4], encode(transform(source, 0, 0, source.width, source.height, argv[2])));
            return 0;
        }
        if (action == "--compare" && argc == 4) {
            const auto a = decode(load(argv[2]));
            const auto b = decode(load(argv[3]));
            if (a.width != b.width || a.height != b.height) {
                std::cout << "Image dimensions differ\n";
                return 1;
            }
            std::size_t different = 0;
            int largest = 0;
            for (std::size_t i = 0; i < a.rgb.size(); i += 3) {
                const int delta = std::max({std::abs(int(a.rgb[i]) - b.rgb[i]),
                                            std::abs(int(a.rgb[i + 1]) - b.rgb[i + 1]),
                                            std::abs(int(a.rgb[i + 2]) - b.rgb[i + 2])});
                different += delta != 0;
                largest = std::max(largest, delta);
            }
            std::cout << "Dimensions " << a.width << "x" << a.height
                      << ", different pixels " << different << ", max channel delta " << largest << '\n';
            return different != 0;
        }
        if (action == "--help") {
            std::cout << "tileflow [--port 8080] [--image disk.img] [--disk-mib 256]\n"
                      << "         [--workers 4] [--tile-size 256] [--demo-delay-ms 0]\n"
                      << "         [--policy priority|fifo] [--web DIR]\n"
                      << "tileflow --serial grayscale|invert|blur INPUT OUTPUT.png\n"
                      << "tileflow --compare FIRST.png SECOND.png\n";
            return 0;
        }
        Config options;
        options.executable = std::filesystem::read_symlink("/proc/self/exe");
        options.web = TILEFLOW_WEB_DIR;
        for (int i = 1; i < argc; i += 2) {
            const std::string flag = argv[i];
            if (i + 1 >= argc) throw std::invalid_argument("Missing value for " + flag);
            const std::string value = argv[i + 1];
            if (flag == "--port") options.port = number(value, 1, 65535);
            else if (flag == "--image") options.disk = value;
            else if (flag == "--disk-mib") options.disk_mib = number(value, 4, 4096);
            else if (flag == "--workers") options.workers = number(value, 1, 16);
            else if (flag == "--tile-size") options.tile_size = number(value, 64, 512);
            else if (flag == "--demo-delay-ms") options.delay_ms = number(value, 0, 5000);
            else if (flag == "--policy" && (value == "fifo" || value == "priority")) options.fifo = value == "fifo";
            else if (flag == "--web") options.web = value;
            else throw std::invalid_argument("Invalid option: " + flag);
        }
        return serve(options);
    } catch (const std::exception& e) {
        std::cerr << "tileflow: " << e.what() << '\n';
        return 1;
    }
}
