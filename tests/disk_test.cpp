#include "tileflow/disk.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <system_error>

#include <unistd.h>

namespace {

using tileflow::Bytes;
using tileflow::Disk;
using tileflow::DiskStats;

constexpr std::size_t block_size = 4096;

class TempImage {
public:
    TempImage() {
        char name[] = "/tmp/tileflow-disk-test-XXXXXX";
        const auto fd = ::mkstemp(name);
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(), "create test image");
        }
        ::close(fd);
        path = name;
    }

    ~TempImage() { ::unlink(path.c_str()); }

    std::string path;
};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Operation>
void expectError(std::errc code, Operation operation) {
    try {
        operation();
    } catch (const std::system_error& error) {
        if (error.code() == code) {
            return;
        }
        throw;
    }
    throw std::runtime_error("operation unexpectedly succeeded");
}

bool sameStats(DiskStats left, DiskStats right) {
    return left.total_blocks == right.total_blocks && left.free_blocks == right.free_blocks &&
           left.total_inodes == right.total_inodes && left.free_inodes == right.free_inodes;
}

void persistenceAndRecycling() {
    TempImage image;
    constexpr std::size_t capacity = 72 * 1024 * 1024 + 7;
    Bytes large(64 * 1024 * 1024 + 37);
    for (std::size_t i = 0; i < large.size(); ++i) {
        large[i] = static_cast<std::uint8_t>(i ^ (i >> 12U) ^ (i >> 22U));
    }
    const std::string longest(255, 'n');
    std::vector<std::string> expected_names{longest};
    DiskStats baseline{};
    DiskStats populated{};
    {
        Disk disk(image.path, capacity);
        baseline = disk.stats();
        disk.mkdir("/job");
        disk.mkdir("/job/tiles");
        disk.mkdir("/job/tiles");
        disk.put("/job/raw", large);
        disk.put("/job/tiles/" + longest, {1, 0, 255});
        for (int i = 0; i < 384; ++i) {
            auto name = "tile-" + std::to_string(i);
            disk.put("/job/tiles/" + name, {static_cast<std::uint8_t>(i), 0});
            expected_names.push_back(std::move(name));
        }
        std::sort(expected_names.begin(), expected_names.end());
        require(std::filesystem::file_size(image.path) == capacity, "disk image size changed");
        expectError(std::errc::directory_not_empty, [&] { disk.remove("/job"); });
        expectError(std::errc::resource_unavailable_try_again, [&] { Disk duplicate(image.path, 0); });
        populated = disk.stats();
    }
    {
        Disk disk(image.path, block_size);
        require(sameStats(disk.stats(), populated), "reopened bitmap or inode statistics differ");
        require(disk.read("/job/raw") == large, "indirect file bytes did not survive reopening");
        require(disk.read("//job/tiles//" + longest) == Bytes({1, 0, 255}), "invalid binary bytes");
        require(disk.list("/job/tiles") == expected_names, "directory entries did not survive reopening");
        require(!disk.exists("/job/raw/absent"), "nested entry under a file exists");
        for (const auto& name : expected_names) {
            disk.remove("/job/tiles/" + name);
        }
        disk.remove("/job/tiles");
        disk.remove("/job/raw");
        disk.remove("/job");
        require(sameStats(disk.stats(), baseline), "deletion leaked a block or inode");
    }
    {
        Disk disk(image.path, 0);
        require(disk.list("/").empty(), "persistent root retained deleted entries");
        require(sameStats(disk.stats(), baseline), "reopened disk lost recycled space");
    }
}

void blockExhaustion() {
    TempImage image;
    Disk disk(image.path, 12 * block_size);
    disk.mkdir("/in");
    const Bytes original{0, 100, 255};
    disk.put("/in/original", original);
    disk.put("/in/filler", Bytes((disk.stats().free_blocks - 1) * block_size, 10));
    for (int i = 0; i < 13; ++i) {
        disk.put("/in/empty-" + std::to_string(i), {});
    }
    const auto full = disk.stats();
    require(disk.list("/in").size() == 15 && full.free_blocks == 1, "invalid exhaustion setup");
    expectError(std::errc::no_space_on_device, [&] { disk.put("/in/original", Bytes(2 * block_size, 7)); });
    expectError(std::errc::no_space_on_device, [&] { disk.put("/in/new", {7}); });
    require(disk.read("/in/original") == original, "failed replacement damaged the existing file");
    require(!disk.exists("/in/new"), "failed creation left a directory entry");
    require(sameStats(disk.stats(), full), "failed write reserved disk space");
    disk.remove("/in/filler");
    const Bytes replacement(2 * block_size, 7);
    disk.put("/in/original", replacement);
    require(disk.read("/in/original") == replacement, "freed blocks could not be reused");
}

void inodeExhaustion() {
    TempImage image;
    Disk disk(image.path, 16 * block_size);
    const auto inodes = disk.stats().free_inodes;
    for (std::size_t i = 0; i < inodes; ++i) {
        disk.put("/empty-" + std::to_string(i), {});
    }
    const auto full = disk.stats();
    require(full.free_inodes == 0 && full.free_blocks > 0, "invalid inode exhaustion setup");
    expectError(std::errc::no_space_on_device, [&] { disk.mkdir("/new"); });
    require(sameStats(disk.stats(), full), "failed inode allocation changed disk stats");
    disk.remove("/empty-0");
    disk.mkdir("/new");
    require(disk.read("/empty-1").empty(), "zero-byte file changed after inode reuse");
    require(disk.exists("/new"), "freed inode could not be reused");
}

} // namespace

int main() {
    try {
        persistenceAndRecycling();
        blockExhaustion();
        inodeExhaustion();
        std::cout << "disk tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
