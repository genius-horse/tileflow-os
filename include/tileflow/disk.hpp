#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tileflow {

using Bytes = std::vector<std::uint8_t>;

struct DiskStats {
    std::size_t total_blocks;
    std::size_t free_blocks;
    std::size_t total_inodes;
    std::size_t free_inodes;
};

// A fixed-size disk image with one owner and serialized operations. The capacity
// applies when creating an empty image. Block statistics count usable data blocks.
class Disk {
public:
    Disk(const std::string& path, std::size_t capacity_bytes);
    ~Disk();

    Disk(const Disk&) = delete;
    Disk& operator=(const Disk&) = delete;

    void mkdir(const std::string& absolute_path);
    void put(const std::string& absolute_path, const Bytes& bytes);
    Bytes read(const std::string& absolute_path) const;
    bool exists(const std::string& absolute_path) const;
    std::vector<std::string> list(const std::string& absolute_path) const;
    void remove(const std::string& absolute_path);
    DiskStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tileflow
