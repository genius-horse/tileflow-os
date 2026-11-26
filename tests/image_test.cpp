#include "tileflow/worker.hpp"

#include <cerrno>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace tileflow;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Image fixture(int w, int h) {
    Image image{w, h, Bytes(static_cast<std::size_t>(w) * h * 3)};
    std::uint32_t state = 29;
    for (auto& channel : image.rgb) {
        state = 1664525u * state + 1013904223u;
        channel = static_cast<std::uint8_t>(state >> 24);
    }
    return image;
}

void test_tiles(const Image& input, int tile_size, const std::string& op) {
    auto tiled = input;
    for (auto tile : split(input.width, input.height, tile_size)) {
        const auto work = make_work(input, tile, op, 1);
        paste(tiled, tile, transform(work.patch, work.x, work.y, work.w, work.h, work.op));
    }
    const auto full = transform(input, 0, 0, input.width, input.height, op);
    require(full.rgb == tiled.rgb, "tile boundaries changed the result");
}

class Child {
public:
    Child() : process(spawn_worker(std::filesystem::canonical("/proc/self/exe"))) {}
    ~Child() {
        if (process.fd >= 0) ::close(process.fd);
        if (process.pid > 0) {
            ::kill(process.pid, SIGKILL);
            while (::waitpid(process.pid, nullptr, 0) < 0 && errno == EINTR) {}
        }
    }

    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    WorkerProcess process;
};

void test_worker() {
    Child child;
    const auto input = fixture(601, 503);
    const auto work = make_work(input, {4, 4, 596, 498}, "invert", 0x12345678abcdefabULL);
    const auto full = transform(work.patch, work.x, work.y, work.w, work.h, work.op);
    require(send_work(child.process.fd, work), "large work frame was not sent");
    Result result;
    require(recv_result(child.process.fd, result), "large result frame was not received");
    require(result.attempt == work.attempt && result.error.empty(), "incorrect result metadata");
    require(result.image.width == work.w && result.image.height == work.h &&
                result.image.rgb == full.rgb,
            "incorrect result pixels");

    const auto small = make_work(input, {0, 0, 1, 1}, "grayscale", 2);
    require(send_work(child.process.fd, small) && recv_result(child.process.fd, result) &&
                result.attempt == 2 && result.image.rgb.size() == 3,
            "worker did not accept a second request");

    require(::kill(child.process.pid, SIGKILL) == 0, "worker could not be killed");
    int status = 0;
    pid_t reaped;
    do {
        reaped = ::waitpid(child.process.pid, &status, 0);
    } while (reaped < 0 && errno == EINTR);
    if (reaped == child.process.pid) child.process.pid = -1;
    require(reaped > 0 && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
            "worker could not be reaped");
    require(!send_work(child.process.fd, small) && !recv_result(child.process.fd, result),
            "closed transport did not report failure");
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--worker") return worker_main(std::stoi(argv[2]));
    try {
        const Image red{1, 1, Bytes{0, 0, 255}};
        require(transform(red, 0, 0, 1, 1, "grayscale").rgb == Bytes{76, 76, 76},
                "grayscale did not apply BGR luminance weights");
        require(transform(red, 0, 0, 1, 1, "invert").rgb == Bytes{255, 255, 0},
                "invert did not flip every channel");

        Image impulse{9, 9, Bytes(9 * 9 * 3)};
        impulse.rgb[3 * (4 * 9 + 4)] = 255;
        const auto blurred = transform(impulse, 0, 0, 9, 9, "blur");
        require(blurred.rgb[3 * (4 * 9 + 4)] == 36 && blurred.rgb[3 * (4 * 9 + 5)] == 24 &&
                    blurred.rgb[3 * (4 * 9 + 6)] == 6 && blurred.rgb[3 * (4 * 9 + 7)] == 0,
                "blur did not use the radius-two Gaussian kernel");

        const auto input = fixture(137, 101);
        const auto decoded = decode(encode(input));
        require(decoded.width == input.width && decoded.height == input.height &&
                    decoded.rgb == input.rgb,
                "PNG round trip changed the image");
        for (const auto* op : {"grayscale", "invert", "blur"}) test_tiles(input, 32, op);
        test_tiles(fixture(11, 8), 1, "blur");
        test_tiles(fixture(1, 23), 3, "blur");
        test_tiles(fixture(23, 2), 3, "blur");
        test_worker();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
