#pragma once

#include "tileflow/disk.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <sys/types.h>
#include <vector>

namespace tileflow {

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct Image {
    int width = 0;
    int height = 0;
    // Tightly packed BGR, in OpenCV channel order, with no row padding.
    std::vector<std::uint8_t> rgb;
};

Image decode(const Bytes& bytes);
Bytes encode(const Image& image);
std::vector<Rect> split(int width, int height, int tile_size);
Image crop_for(const Image& image, Rect rect, const std::string& op);
Image transform(const Image& patch, int core_x, int core_y, int w, int h,
                const std::string& op);
void paste(Image& destination, Rect rect, const Image& tile);
bool supported_operation(const std::string& op);

struct Work {
    std::uint64_t attempt = 0;
    Image patch;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    std::string op;
    int delay_ms = 0;
};

struct Result {
    std::uint64_t attempt = 0;
    Image image;
    std::string error;
};

Work make_work(const Image& image, Rect rect, const std::string& op,
               std::uint64_t attempt, int delay_ms = 0);

struct WorkerProcess {
    pid_t pid = -1;
    int fd = -1;
};

// The caller owns the returned descriptor and must reap the child.
// The executable must dispatch `--worker FD` to worker_main.
WorkerProcess spawn_worker(const std::filesystem::path& executable);
// A single caller must serialize writes to the same socket. False means the
// transport is unusable, including EOF, a truncated frame, or invalid data.
bool send_work(int fd, const Work& work);
bool recv_result(int fd, Result& result);
int worker_main(int fd);

} // namespace tileflow
