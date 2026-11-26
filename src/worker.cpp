#include "tileflow/worker.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <spawn.h>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <sys/socket.h>
#include <unistd.h>

extern char** environ;

namespace tileflow {
namespace {

constexpr std::size_t max_pixels = 16 * 1024 * 1024;
constexpr int blur_radius = 2;
constexpr std::uint32_t work_magic = 0x54465731;   // TFW1
constexpr std::uint32_t result_magic = 0x54465231; // TFR1
constexpr std::size_t work_fields = 40;
constexpr std::size_t result_fields = 20;
constexpr std::size_t max_error_bytes = 1024;

enum class Operation : std::uint32_t { invalid, grayscale, invert, blur };

Operation operation_id(const std::string& op) {
    if (op == "grayscale") return Operation::grayscale;
    if (op == "invert") return Operation::invert;
    if (op == "blur") return Operation::blur;
    return Operation::invalid;
}

const char* operation_name(std::uint32_t id) {
    switch (static_cast<Operation>(id)) {
        case Operation::grayscale: return "grayscale";
        case Operation::invert: return "invert";
        case Operation::blur: return "blur";
        case Operation::invalid: break;
    }
    return nullptr;
}

std::size_t image_bytes(int width, int height) {
    if (width <= 0 || height <= 0 ||
        static_cast<std::size_t>(width) > max_pixels / static_cast<std::size_t>(height)) {
        return 0;
    }
    return 3 * static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
}

bool valid_image(const Image& image) {
    const auto expected = image_bytes(image.width, image.height);
    return expected != 0 && image.rgb.size() == expected;
}

bool fits(Rect rect, const Image& image) {
    return rect.x >= 0 && rect.y >= 0 && rect.w > 0 && rect.h > 0 &&
           rect.w <= image.width && rect.h <= image.height &&
           rect.x <= image.width - rect.w && rect.y <= image.height - rect.h;
}

cv::Mat as_mat(const Image& image) {
    if (!valid_image(image)) throw std::invalid_argument("invalid image dimensions or pixels");
    // OpenCV has no const-data Mat constructor. Callers use this as input only.
    return cv::Mat(image.height, image.width, CV_8UC3,
                   const_cast<std::uint8_t*>(image.rgb.data()));
}

Image copy_image(const cv::Mat& mat) {
    const auto size = image_bytes(mat.cols, mat.rows);
    if (size == 0 || mat.type() != CV_8UC3) {
        throw std::invalid_argument("image must be nonempty and at most 16,777,216 pixels");
    }
    Image image{mat.cols, mat.rows, Bytes(size)};
    const auto stride = static_cast<std::size_t>(mat.cols) * 3;
    for (int row = 0; row < mat.rows; ++row) {
        std::memcpy(image.rgb.data() + row * stride, mat.ptr(row), stride);
    }
    return image;
}

Rect patch_rect(const Image& image, Rect core, const std::string& op) {
    const auto operation = operation_id(op);
    if (operation == Operation::invalid) throw std::invalid_argument("unsupported operation");
    if (!fits(core, image)) throw std::invalid_argument("tile is outside the image");
    if (operation != Operation::blur) return core;

    const int left = std::max(0, core.x - blur_radius);
    const int top = std::max(0, core.y - blur_radius);
    int right = core.x + core.w;
    int bottom = core.y + core.h;
    right += std::min(blur_radius, image.width - right);
    bottom += std::min(blur_radius, image.height - bottom);
    return {left, top, right - left, bottom - top};
}

cv::Rect as_roi(Rect rect) {
    return cv::Rect(rect.x, rect.y, rect.w, rect.h);
}

bool send_all(int fd, const void* data, std::size_t size) {
    auto* cursor = static_cast<const std::uint8_t*>(data);
    while (size != 0) {
        const auto sent = ::send(fd, cursor, size, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        cursor += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool recv_all(int fd, void* data, std::size_t size) {
    auto* cursor = static_cast<std::uint8_t*>(data);
    while (size != 0) {
        const auto received = ::recv(fd, cursor, size, 0);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return false;
        cursor += received;
        size -= static_cast<std::size_t>(received);
    }
    return true;
}

void put_u32(std::uint8_t* p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>(value >> 24);
    p[1] = static_cast<std::uint8_t>(value >> 16);
    p[2] = static_cast<std::uint8_t>(value >> 8);
    p[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t get_u32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

void put_u64(std::uint8_t* p, std::uint64_t value) {
    put_u32(p, static_cast<std::uint32_t>(value >> 32));
    put_u32(p + 4, static_cast<std::uint32_t>(value));
}

std::uint64_t get_u64(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>(get_u32(p)) << 32) | get_u32(p + 4);
}

bool get_int(const std::uint8_t* p, int& out) {
    const auto value = get_u32(p);
    if (value > static_cast<std::uint32_t>(INT32_MAX)) return false;
    out = static_cast<int>(value);
    return true;
}

template<std::size_t field_count>
bool send_header(int fd, std::uint32_t magic,
                 const std::array<std::uint8_t, field_count>& fields, std::size_t tail_size) {
    std::array<std::uint8_t, 8> prefix{};
    put_u32(prefix.data(), magic);
    put_u32(prefix.data() + 4, static_cast<std::uint32_t>(fields.size() + tail_size));
    return send_all(fd, prefix.data(), prefix.size()) &&
           send_all(fd, fields.data(), fields.size());
}

template<std::size_t field_count>
bool recv_header(int fd, std::uint32_t magic,
                 std::array<std::uint8_t, field_count>& fields, std::size_t& tail_size) {
    std::array<std::uint8_t, 8> prefix{};
    if (!recv_all(fd, prefix.data(), prefix.size()) || get_u32(prefix.data()) != magic) {
        return false;
    }
    const auto frame_size = get_u32(prefix.data() + 4);
    if (frame_size < fields.size() || frame_size - fields.size() > 3 * max_pixels) return false;
    tail_size = frame_size - fields.size();
    return recv_all(fd, fields.data(), fields.size());
}

bool recv_work(int fd, Work& work) {
    Work next;
    std::array<std::uint8_t, work_fields> fields{};
    std::size_t size = 0;
    if (!recv_header(fd, work_magic, fields, size)) return false;
    next.attempt = get_u64(fields.data());
    const char* op = operation_name(get_u32(fields.data() + 32));
    if (!op || !get_int(fields.data() + 8, next.patch.width) ||
        !get_int(fields.data() + 12, next.patch.height) ||
        !get_int(fields.data() + 16, next.x) || !get_int(fields.data() + 20, next.y) ||
        !get_int(fields.data() + 24, next.w) || !get_int(fields.data() + 28, next.h) ||
        !get_int(fields.data() + 36, next.delay_ms) ||
        size != image_bytes(next.patch.width, next.patch.height) ||
        !fits({next.x, next.y, next.w, next.h}, next.patch)) {
        return false;
    }
    next.op = op;
    next.patch.rgb.resize(size);
    if (!recv_all(fd, next.patch.rgb.data(), size)) return false;
    work = std::move(next);
    return true;
}

bool send_result(int fd, const Result& result) {
    std::array<std::uint8_t, result_fields> fields{};
    put_u64(fields.data(), result.attempt);
    if (!result.error.empty()) {
        const auto size = std::min(result.error.size(), max_error_bytes);
        put_u32(fields.data() + 8, 1);
        return send_header(fd, result_magic, fields, size) &&
               send_all(fd, result.error.data(), size);
    }
    if (!valid_image(result.image)) return false;
    put_u32(fields.data() + 12, static_cast<std::uint32_t>(result.image.width));
    put_u32(fields.data() + 16, static_cast<std::uint32_t>(result.image.height));
    return send_header(fd, result_magic, fields, result.image.rgb.size()) &&
           send_all(fd, result.image.rgb.data(), result.image.rgb.size());
}

} // namespace

Image decode(const Bytes& bytes) {
    if (bytes.empty()) throw std::invalid_argument("image upload is empty");
    const auto mat = cv::imdecode(bytes, cv::IMREAD_COLOR);
    if (mat.empty()) throw std::invalid_argument("image could not be decoded");
    return copy_image(mat);
}

Bytes encode(const Image& image) {
    Bytes bytes;
    if (!cv::imencode(".png", as_mat(image), bytes)) throw std::runtime_error("PNG encoding failed");
    return bytes;
}

std::vector<Rect> split(int width, int height, int tile_size) {
    if (image_bytes(width, height) == 0 || tile_size <= 0) {
        throw std::invalid_argument("invalid image dimensions or tile size");
    }
    std::vector<Rect> tiles;
    for (int y = 0; y < height;) {
        const int h = std::min(tile_size, height - y);
        for (int x = 0; x < width;) {
            const int w = std::min(tile_size, width - x);
            tiles.push_back({x, y, w, h});
            x += w;
        }
        y += h;
    }
    return tiles;
}

Image crop_for(const Image& image, Rect rect, const std::string& op) {
    return copy_image(as_mat(image)(as_roi(patch_rect(image, rect, op))));
}

Image transform(const Image& patch, int core_x, int core_y, int w, int h,
                const std::string& op) {
    const Rect core{core_x, core_y, w, h};
    if (!fits(core, patch)) throw std::invalid_argument("tile is outside the patch");
    const auto src = as_mat(patch);
    cv::Mat dst;
    switch (operation_id(op)) {
        case Operation::grayscale: {
            cv::Mat gray;
            cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
            cv::cvtColor(gray, dst, cv::COLOR_GRAY2BGR);
            break;
        }
        case Operation::invert:
            cv::bitwise_not(src, dst);
            break;
        case Operation::blur:
            cv::GaussianBlur(src, dst, cv::Size(5, 5), 0, 0, cv::BORDER_REFLECT_101);
            break;
        case Operation::invalid:
            throw std::invalid_argument("unsupported operation");
    }
    return copy_image(dst(as_roi(core)));
}

void paste(Image& destination, Rect rect, const Image& tile) {
    if (!valid_image(destination) || !valid_image(tile) || !fits(rect, destination) ||
        rect.w != tile.width || rect.h != tile.height) {
        throw std::invalid_argument("tile does not fit the destination image");
    }
    const auto tile_stride = 3 * static_cast<std::size_t>(rect.w);
    const auto dst_stride = 3 * static_cast<std::size_t>(destination.width);
    const auto x_offset = 3 * static_cast<std::size_t>(rect.x);
    for (int row = 0; row < rect.h; ++row) {
        std::memmove(destination.rgb.data() + (row + rect.y) * dst_stride + x_offset,
                     tile.rgb.data() + row * tile_stride, tile_stride);
    }
}

bool supported_operation(const std::string& op) {
    return operation_id(op) != Operation::invalid;
}

Work make_work(const Image& image, Rect rect, const std::string& op,
               std::uint64_t attempt, int delay_ms) {
    if (delay_ms < 0) throw std::invalid_argument("worker delay cannot be negative");
    const auto patch = patch_rect(image, rect, op);
    return {attempt, copy_image(as_mat(image)(as_roi(patch))), rect.x - patch.x,
            rect.y - patch.y, rect.w, rect.h, op, delay_ms};
}

WorkerProcess spawn_worker(const std::filesystem::path& executable) {
    constexpr int worker_fd = 3;
    char option[] = "--worker";
    char descriptor[] = "3";
    auto program = executable.string();
    char* argv[] = {program.data(), option, descriptor, nullptr};

    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
        throw std::system_error(errno, std::generic_category(), "socketpair");
    }
    if (sockets[1] == worker_fd) {
        // Keep the source different from the destination so dup2 clears CLOEXEC.
        const int source = ::fcntl(sockets[1], F_DUPFD_CLOEXEC, worker_fd + 1);
        const int error = errno;
        ::close(sockets[1]);
        sockets[1] = source;
        if (source < 0) {
            ::close(sockets[0]);
            throw std::system_error(error, std::generic_category(), "duplicate worker socket");
        }
    }

    pid_t pid = -1;
    posix_spawn_file_actions_t actions;
    int error = ::posix_spawn_file_actions_init(&actions);
    if (error == 0) {
        error = ::posix_spawn_file_actions_addclose(&actions, sockets[0]);
        if (error == 0) error = ::posix_spawn_file_actions_adddup2(&actions, sockets[1], worker_fd);
        if (error == 0) error = ::posix_spawn(&pid, program.c_str(), &actions, nullptr, argv, environ);
        ::posix_spawn_file_actions_destroy(&actions);
    }
    ::close(sockets[1]);
    if (error != 0) {
        ::close(sockets[0]);
        throw std::system_error(error, std::generic_category(), "spawn worker");
    }
    return {pid, sockets[0]};
}

bool send_work(int fd, const Work& work) {
    const auto op = operation_id(work.op);
    if (!valid_image(work.patch) || !fits({work.x, work.y, work.w, work.h}, work.patch) ||
        work.delay_ms < 0 || op == Operation::invalid) {
        return false;
    }

    std::array<std::uint8_t, work_fields> fields{};
    put_u64(fields.data(), work.attempt);
    put_u32(fields.data() + 8, static_cast<std::uint32_t>(work.patch.width));
    put_u32(fields.data() + 12, static_cast<std::uint32_t>(work.patch.height));
    put_u32(fields.data() + 16, static_cast<std::uint32_t>(work.x));
    put_u32(fields.data() + 20, static_cast<std::uint32_t>(work.y));
    put_u32(fields.data() + 24, static_cast<std::uint32_t>(work.w));
    put_u32(fields.data() + 28, static_cast<std::uint32_t>(work.h));
    put_u32(fields.data() + 32, static_cast<std::uint32_t>(op));
    put_u32(fields.data() + 36, static_cast<std::uint32_t>(work.delay_ms));
    return send_header(fd, work_magic, fields, work.patch.rgb.size()) &&
           send_all(fd, work.patch.rgb.data(), work.patch.rgb.size());
}

bool recv_result(int fd, Result& result) {
    try {
        Result next;
        std::array<std::uint8_t, result_fields> fields{};
        std::size_t size = 0;
        if (!recv_header(fd, result_magic, fields, size)) return false;
        next.attempt = get_u64(fields.data());
        const auto status = get_u32(fields.data() + 8);
        if (!get_int(fields.data() + 12, next.image.width) ||
            !get_int(fields.data() + 16, next.image.height) || size == 0) {
            return false;
        }
        if (status == 1 && next.image.width == 0 && next.image.height == 0 &&
            size <= max_error_bytes) {
            next.error.resize(size);
            if (!recv_all(fd, next.error.data(), size)) return false;
        } else if (status == 0 && size == image_bytes(next.image.width, next.image.height)) {
            next.image.rgb.resize(size);
            if (!recv_all(fd, next.image.rgb.data(), size)) return false;
        } else {
            return false;
        }
        result = std::move(next);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

int worker_main(int fd) {
    cv::setNumThreads(1);
    Work work;
    while (recv_work(fd, work)) {
        Result result;
        result.attempt = work.attempt;
        try {
            if (work.delay_ms != 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(work.delay_ms));
            }
            result.image = transform(work.patch, work.x, work.y, work.w, work.h, work.op);
        } catch (const std::exception& error) {
            result.error = error.what();
            if (result.error.empty()) result.error = "worker operation failed";
        }
        if (!send_result(fd, result)) break;
    }
    ::close(fd);
    return 0;
}

} // namespace tileflow
