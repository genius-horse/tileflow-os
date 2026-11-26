#include "tileflow/disk.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tileflow {
namespace {

using Id = std::uint32_t;
constexpr std::size_t block_size = 4096;
constexpr std::size_t direct_slots = 12;
constexpr std::size_t index_slots = block_size / sizeof(Id);
constexpr std::size_t max_file_blocks = direct_slots + index_slots + index_slots * index_slots;
constexpr std::size_t max_name_length = 255;
constexpr std::array<char, 8> disk_magic{'T', 'I', 'L', 'E', 'F', 'S', '1', '\0'};
constexpr Id disk_version = 1;
constexpr Id root = 0;

using Block = std::array<Id, index_slots>;

struct Superblock {
    std::array<char, 8> magic;
    Id version;
    Id bytes_per_block;
    Id block_count;
    Id inode_count;
    Id inode_start;
    Id bitmap_start;
    Id data_start;
    Id reserved;
    std::uint64_t capacity_bytes;
};

enum class InodeKind : Id { free, file, directory };

struct Inode {
    std::uint64_t size = 0;
    InodeKind kind = InodeKind::free;
    std::array<Id, direct_slots> direct{};
    Id single = 0;
    Id doubly = 0;
    std::array<Id, 15> reserved{};
};

struct DirectoryEntry {
    Id inode;
    char name[max_name_length + 1];
};

static_assert(sizeof(Superblock) == 48);
static_assert(sizeof(Inode) == 128);
static_assert(sizeof(DirectoryEntry) == 260);
static_assert(sizeof(Block) == block_size);
static_assert(sizeof(off_t) >= 8);
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the disk format uses little-endian fields");
#endif

[[noreturn]] void fail(int error, const std::string& description) {
    throw std::system_error(error, std::generic_category(), description);
}

std::uint64_t roundUp(std::uint64_t size, std::uint64_t divisor) {
    return size / divisor + (size % divisor != 0);
}

std::uint64_t offsetOf(Id block) {
    return static_cast<std::uint64_t>(block) * block_size;
}

std::size_t bitmapBytes(Id blocks) {
    return static_cast<std::size_t>(roundUp(blocks, 8));
}

bool allocated(const Bytes& bitmap, Id id) {
    return (bitmap[id / 8] & (1U << (id % 8))) != 0;
}

void mark(Bytes& bitmap, Id id, bool in_use) {
    auto& byte = bitmap[id / 8];
    const auto mask = static_cast<std::uint8_t>(1U << (id % 8));
    byte = in_use ? byte | mask : byte & static_cast<std::uint8_t>(~mask);
}

using Components = std::vector<std::string_view>;

Components splitPath(std::string_view path) {
    if (path.empty() || path.front() != '/' || path.find('\0') != std::string_view::npos) {
        fail(EINVAL, "invalid absolute path");
    }
    Components components;
    auto begin = path.find_first_not_of('/');
    while (begin != std::string_view::npos) {
        const auto end = path.find('/', begin);
        const auto component = path.substr(begin, end - begin);
        if (component.size() > max_name_length) {
            fail(ENAMETOOLONG, "path component");
        }
        if (component == "." || component == "..") {
            fail(EINVAL, "relative path component");
        }
        components.push_back(component);
        begin = end == std::string_view::npos ? end : path.find_first_not_of('/', end);
    }
    return components;
}

using Entries = std::vector<DirectoryEntry>;

Entries::const_iterator findEntry(const Entries& entries, std::string_view name) {
    return std::find_if(entries.begin(), entries.end(), [name](const DirectoryEntry& entry) {
        return name == entry.name;
    });
}

DirectoryEntry makeEntry(Id id, std::string_view name) {
    DirectoryEntry entry{};
    entry.inode = id;
    std::copy(name.begin(), name.end(), entry.name);
    return entry;
}

} // namespace

struct Disk::Impl {
    struct Transaction;
    struct Parent {
        Id id;
        Inode inode;
        Entries entries;
    };

    Impl(const std::string& path, std::size_t capacity);
    ~Impl() { ::close(fd); }

    void readExact(void* buffer, std::size_t size, std::uint64_t offset) const;
    void writeExact(const void* buffer, std::size_t size, std::uint64_t offset) const;
    void initialize(std::size_t capacity);
    void load(std::uint64_t size);
    std::uint64_t dataOffset(Id id) const;
    std::uint64_t inodeOffset(Id id) const;
    Block readBlock(Id id) const;
    Id readIndex(Id id, std::size_t slot) const;
    Id dataBlock(const Inode& inode, std::size_t slot) const;
    void readData(const Inode& inode, void* destination, std::size_t size) const;
    std::size_t checkedSize(const Inode& inode) const;
    Entries readDirectory(const Inode& inode) const;
    std::optional<Id> resolve(const Components& path, std::size_t count) const;
    Id lookup(const Components& path) const;
    Parent parentOf(const Components& path) const;
    Id freeInode() const;

    int fd;
    Superblock super{};
    std::vector<Inode> inodes;
    Bytes bitmap;
    Id block_hint = 0;
    Id inode_hint = 1;
    mutable std::mutex mutex;
};

Disk::Impl::Impl(const std::string& path, std::size_t capacity)
    : fd(::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600)) {
    if (fd < 0) {
        fail(errno, "open disk image " + path);
    }
    try {
        if (::flock(fd, LOCK_EX | LOCK_NB) < 0) {
            fail(errno, "lock disk image " + path);
        }
        struct stat status {};
        if (::fstat(fd, &status) < 0) {
            fail(errno, "stat disk image " + path);
        }
        if (!S_ISREG(status.st_mode)) {
            fail(EINVAL, "disk image must be a regular file");
        }
        if (status.st_size == 0) {
            try {
                initialize(capacity);
            } catch (...) {
                const auto reset = ::ftruncate(fd, 0);
                (void)reset;
                throw;
            }
        } else {
            load(static_cast<std::uint64_t>(status.st_size));
        }
    } catch (...) {
        ::close(fd);
        throw;
    }
}

void Disk::Impl::readExact(void* buffer, std::size_t size, std::uint64_t offset) const {
    auto* output = static_cast<std::uint8_t*>(buffer);
    while (size != 0) {
        const auto count = ::pread(fd, output, size, static_cast<off_t>(offset));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail(errno, "read disk image");
        }
        if (count == 0) {
            fail(EIO, "unexpected end of disk image");
        }
        const auto transferred = static_cast<std::size_t>(count);
        output += transferred;
        size -= transferred;
        offset += transferred;
    }
}

void Disk::Impl::writeExact(const void* buffer, std::size_t size, std::uint64_t offset) const {
    const auto* input = static_cast<const std::uint8_t*>(buffer);
    while (size != 0) {
        const auto count = ::pwrite(fd, input, size, static_cast<off_t>(offset));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail(errno, "write disk image");
        }
        if (count == 0) {
            fail(EIO, "incomplete disk image write");
        }
        const auto transferred = static_cast<std::size_t>(count);
        input += transferred;
        size -= transferred;
        offset += transferred;
    }
}

void Disk::Impl::initialize(std::size_t capacity) {
    const auto blocks = capacity / block_size;
    if (blocks < 8 || blocks >= std::numeric_limits<Id>::max()) {
        fail(EINVAL, "disk capacity is outside the supported range");
    }
    const auto inode_count = std::clamp<std::size_t>(blocks, 64, 262144);
    const auto bitmap_start = 1 + roundUp(inode_count * sizeof(Inode), block_size);
    const auto data_start = bitmap_start + roundUp(bitmapBytes(static_cast<Id>(blocks)), block_size);
    super = {disk_magic, disk_version, block_size, static_cast<Id>(blocks),
             static_cast<Id>(inode_count), 1, static_cast<Id>(bitmap_start),
             static_cast<Id>(data_start), 0, capacity};
    inodes.resize(super.inode_count);
    inodes[root].kind = InodeKind::directory;
    bitmap.resize(bitmapBytes(super.block_count));
    for (Id id = 0; id < super.data_start; ++id) {
        mark(bitmap, id, true);
    }
    if (::ftruncate(fd, static_cast<off_t>(capacity)) < 0) {
        fail(errno, "set disk image capacity");
    }
    writeExact(&inodes[root], sizeof(Inode), inodeOffset(root));
    writeExact(bitmap.data(), bitmap.size(), offsetOf(super.bitmap_start));
    writeExact(&super, sizeof(super), 0);
    block_hint = super.data_start;
}

void Disk::Impl::load(std::uint64_t size) {
    readExact(&super, sizeof(super), 0);
    if (super.magic != disk_magic || super.version != disk_version ||
        super.bytes_per_block != block_size || super.capacity_bytes != size ||
        super.block_count != size / block_size ||
        super.block_count == std::numeric_limits<Id>::max() || super.inode_start != 1 ||
        super.inode_count < 64 || super.inode_count > 262144 ||
        super.bitmap_start != 1 + roundUp(static_cast<std::uint64_t>(super.inode_count) *
                                             sizeof(Inode), block_size) ||
        super.data_start != super.bitmap_start + roundUp(bitmapBytes(super.block_count), block_size) ||
        super.data_start >= super.block_count) {
        fail(EINVAL, "invalid disk image format");
    }
    inodes.resize(super.inode_count);
    readExact(inodes.data(), inodes.size() * sizeof(Inode), inodeOffset(root));
    bitmap.resize(bitmapBytes(super.block_count));
    readExact(bitmap.data(), bitmap.size(), offsetOf(super.bitmap_start));
    if (inodes[root].kind != InodeKind::directory) {
        fail(EIO, "invalid root inode");
    }
    block_hint = super.data_start;
}

std::uint64_t Disk::Impl::dataOffset(Id id) const {
    if (id < super.data_start || id >= super.block_count) {
        fail(EIO, "invalid data block pointer");
    }
    return offsetOf(id);
}

std::uint64_t Disk::Impl::inodeOffset(Id id) const {
    return offsetOf(super.inode_start) + static_cast<std::uint64_t>(id) * sizeof(Inode);
}

Block Disk::Impl::readBlock(Id id) const {
    Block storage;
    readExact(storage.data(), block_size, dataOffset(id));
    return storage;
}

Id Disk::Impl::readIndex(Id id, std::size_t slot) const {
    Id pointer;
    readExact(&pointer, sizeof(pointer), dataOffset(id) + slot * sizeof(pointer));
    return pointer;
}

Id Disk::Impl::dataBlock(const Inode& inode, std::size_t slot) const {
    if (slot < direct_slots) {
        return inode.direct[slot];
    }
    slot -= direct_slots;
    if (slot < index_slots) {
        return readIndex(inode.single, slot);
    }
    slot -= index_slots;
    const auto index = readIndex(inode.doubly, slot / index_slots);
    return readIndex(index, slot % index_slots);
}

void Disk::Impl::readData(const Inode& inode, void* destination, std::size_t size) const {
    auto* output = static_cast<std::uint8_t*>(destination);
    for (std::size_t slot = 0; size != 0; ++slot) {
        const auto part = std::min(size, block_size);
        readExact(output, part, dataOffset(dataBlock(inode, slot)));
        output += part;
        size -= part;
    }
}

std::size_t Disk::Impl::checkedSize(const Inode& inode) const {
    const auto max_blocks = std::min<std::uint64_t>(max_file_blocks,
                                                    super.block_count - super.data_start);
    if (inode.size > max_blocks * block_size || inode.size > std::numeric_limits<std::size_t>::max()) {
        fail(EIO, "invalid inode size");
    }
    return static_cast<std::size_t>(inode.size);
}

Entries Disk::Impl::readDirectory(const Inode& inode) const {
    if (inode.kind != InodeKind::directory) {
        fail(ENOTDIR, "read directory");
    }
    const auto size = checkedSize(inode);
    if (size % sizeof(DirectoryEntry) != 0 || size / sizeof(DirectoryEntry) >= inodes.size()) {
        fail(EIO, "invalid directory size");
    }
    Entries entries(size / sizeof(DirectoryEntry));
    readData(inode, entries.data(), size);
    for (const auto& entry : entries) {
        if (entry.inode == root || entry.inode >= inodes.size() ||
            inodes[entry.inode].kind == InodeKind::free || entry.name[0] == '\0' ||
            !std::memchr(entry.name, '\0', sizeof(entry.name))) {
            fail(EIO, "invalid directory entry");
        }
    }
    return entries;
}

std::optional<Id> Disk::Impl::resolve(const Components& path, std::size_t count) const {
    Id current = root;
    for (std::size_t i = 0; i < count; ++i) {
        const auto entries = readDirectory(inodes[current]);
        const auto entry = findEntry(entries, path[i]);
        if (entry == entries.end()) {
            return std::nullopt;
        }
        current = entry->inode;
    }
    return current;
}

Id Disk::Impl::lookup(const Components& path) const {
    const auto result = resolve(path, path.size());
    if (!result) {
        fail(ENOENT, "path does not exist");
    }
    return *result;
}

Disk::Impl::Parent Disk::Impl::parentOf(const Components& path) const {
    const auto id = resolve(path, path.size() - 1);
    if (!id) {
        fail(ENOENT, "parent directory does not exist");
    }
    return {*id, inodes[*id], readDirectory(inodes[*id])};
}

Id Disk::Impl::freeInode() const {
    for (Id id = inode_hint; id < inodes.size(); ++id) {
        if (inodes[id].kind == InodeKind::free) {
            return id;
        }
    }
    fail(ENOSPC, "disk inode table is full");
}

// Allocation and index changes are staged until all required free blocks and
// inodes have been reserved. Writes to new file data only touch free blocks.
// This provides rollback on allocation failure during normal operation.
struct Disk::Impl::Transaction {
    explicit Transaction(Impl& disk) : disk(disk), bitmap(disk.bitmap), hint(disk.block_hint) {}

    Id allocate() {
        while (hint < disk.super.block_count && allocated(bitmap, hint)) {
            ++hint;
        }
        if (hint == disk.super.block_count) {
            fail(ENOSPC, "disk data region is full");
        }
        const auto id = hint++;
        mark(bitmap, id, true);
        return id;
    }

    void release(Id id) {
        disk.dataOffset(id);
        if (!allocated(bitmap, id)) {
            fail(EIO, "freeing an unused data block");
        }
        mark(bitmap, id, false);
        hint = std::min(hint, id);
        changed_blocks.erase(id);
    }

    Block& edit(Id id) {
        const auto existing = changed_blocks.find(id);
        if (existing != changed_blocks.end()) {
            return existing->second;
        }
        return changed_blocks.emplace(id, disk.readBlock(id)).first->second;
    }

    Block& editIndex(Id& id) {
        if (id == 0) {
            id = allocate();
            return changed_blocks.emplace(id, Block{}).first->second;
        }
        return edit(id);
    }

    Id ensureDataBlock(Inode& inode, std::size_t slot) {
        if (slot < direct_slots) {
            return ensure(inode.direct[slot]);
        }
        slot -= direct_slots;
        if (slot < index_slots) {
            return ensure(editIndex(inode.single)[slot]);
        }
        slot -= index_slots;
        auto& outside = editIndex(inode.doubly);
        auto& inside = editIndex(outside[slot / index_slots]);
        return ensure(inside[slot % index_slots]);
    }

    Id ensure(Id& id) {
        if (id == 0) {
            id = allocate();
        }
        return id;
    }

    void writeEntry(Inode& inode, std::size_t position, const DirectoryEntry& entry) {
        std::size_t offset = position * sizeof(DirectoryEntry);
        const auto old_blocks = roundUp(inode.size, block_size);
        const auto* source = reinterpret_cast<const std::uint8_t*>(&entry);
        auto remaining = sizeof(DirectoryEntry);
        while (remaining != 0) {
            const auto slot = offset / block_size;
            const auto id = ensureDataBlock(inode, slot);
            const auto within = offset % block_size;
            const auto part = std::min(remaining, block_size - within);
            auto& storage = slot < old_blocks ? edit(id) : changed_blocks.emplace(id, Block{}).first->second;
            std::memcpy(reinterpret_cast<std::uint8_t*>(storage.data()) + within, source, part);
            source += part;
            offset += part;
            remaining -= part;
        }
        inode.size = std::max<std::uint64_t>(inode.size, offset);
    }

    void eraseEntry(Inode& inode, const Entries& entries, std::size_t position) {
        if (position + 1 != entries.size()) {
            writeEntry(inode, position, entries.back());
        }
        const auto old_blocks = roundUp(inode.size, block_size);
        inode.size -= sizeof(DirectoryEntry);
        if (roundUp(inode.size, block_size) < old_blocks) {
            releaseLastDataBlock(inode, static_cast<std::size_t>(old_blocks - 1));
        }
    }

    void releaseLastDataBlock(Inode& inode, std::size_t slot) {
        release(disk.dataBlock(inode, slot));
        if (slot < direct_slots) {
            inode.direct[slot] = 0;
            return;
        }
        slot -= direct_slots;
        if (slot < index_slots) {
            if (slot == 0) {
                release(inode.single);
                inode.single = 0;
            } else {
                edit(inode.single)[slot] = 0;
            }
            return;
        }
        slot -= index_slots;
        const auto group = slot / index_slots;
        const auto pointer = disk.readIndex(inode.doubly, group);
        if (slot % index_slots != 0) {
            edit(pointer)[slot % index_slots] = 0;
        } else if (group == 0) {
            release(pointer);
            release(inode.doubly);
            inode.doubly = 0;
        } else {
            release(pointer);
            edit(inode.doubly)[group] = 0;
        }
    }

    void releaseIndex(Id id) {
        const auto pointers = disk.readBlock(id);
        for (const Id pointer : pointers) {
            if (pointer != 0) {
                release(pointer);
            }
        }
        release(id);
    }

    void releaseFile(const Inode& inode) {
        for (const Id id : inode.direct) {
            if (id != 0) {
                release(id);
            }
        }
        if (inode.single != 0) {
            releaseIndex(inode.single);
        }
        if (inode.doubly != 0) {
            const auto outside = disk.readBlock(inode.doubly);
            for (const Id id : outside) {
                if (id != 0) {
                    releaseIndex(id);
                }
            }
            release(inode.doubly);
        }
    }

    void commit(std::initializer_list<std::pair<Id, Inode>> changed_inodes) {
        for (const auto& [id, storage] : changed_blocks) {
            disk.writeExact(storage.data(), block_size, disk.dataOffset(id));
        }
        for (const auto& [id, inode] : changed_inodes) {
            disk.writeExact(&inode, sizeof(inode), disk.inodeOffset(id));
        }
        disk.writeExact(bitmap.data(), bitmap.size(), offsetOf(disk.super.bitmap_start));
        for (const auto& [id, inode] : changed_inodes) {
            disk.inodes[id] = inode;
        }
        disk.bitmap.swap(bitmap);
        disk.block_hint = hint;
    }

    Impl& disk;
    Bytes bitmap;
    Id hint;
    std::unordered_map<Id, Block> changed_blocks;
};

Disk::Disk(const std::string& path, std::size_t capacity_bytes)
    : impl_(std::make_unique<Impl>(path, capacity_bytes)) {}

Disk::~Disk() = default;

void Disk::mkdir(const std::string& path) {
    std::lock_guard lock(impl_->mutex);
    const auto components = splitPath(path);
    if (components.empty()) {
        return;
    }
    auto parent = impl_->parentOf(components);
    const auto entry = findEntry(parent.entries, components.back());
    if (entry != parent.entries.end()) {
        if (impl_->inodes[entry->inode].kind != InodeKind::directory) {
            fail(EEXIST, "path already exists as a file");
        }
        return;
    }
    const auto id = impl_->freeInode();
    Inode directory;
    directory.kind = InodeKind::directory;
    Impl::Transaction transaction(*impl_);
    transaction.writeEntry(parent.inode, parent.entries.size(), makeEntry(id, components.back()));
    transaction.commit({{id, directory}, {parent.id, parent.inode}});
    impl_->inode_hint = id + 1;
}

void Disk::put(const std::string& path, const Bytes& bytes) {
    std::lock_guard lock(impl_->mutex);
    const auto components = splitPath(path);
    if (components.empty()) {
        fail(EISDIR, "write root directory");
    }
    auto parent = impl_->parentOf(components);
    const auto entry = findEntry(parent.entries, components.back());
    const auto replacing = entry != parent.entries.end();
    const auto id = replacing ? entry->inode : impl_->freeInode();
    if (impl_->inodes[id].kind == InodeKind::directory) {
        fail(EISDIR, "write directory");
    }
    const auto slots = roundUp(bytes.size(), block_size);
    if (slots > max_file_blocks) {
        fail(EFBIG, "file exceeds the inode block mapping limit");
    }
    if (slots > impl_->super.block_count - impl_->super.data_start) {
        fail(ENOSPC, "file exceeds the disk data capacity");
    }

    Inode inode;
    inode.kind = InodeKind::file;
    inode.size = bytes.size();
    Impl::Transaction transaction(*impl_);
    if (!replacing) {
        transaction.writeEntry(parent.inode, parent.entries.size(), makeEntry(id, components.back()));
    }
    std::vector<Id> data_blocks;
    data_blocks.reserve(static_cast<std::size_t>(slots));
    for (std::size_t slot = 0; slot < slots; ++slot) {
        data_blocks.push_back(transaction.ensureDataBlock(inode, slot));
    }
    if (replacing) {
        transaction.releaseFile(impl_->inodes[id]);
    }
    std::size_t offset = 0;
    for (const auto block : data_blocks) {
        const auto size = std::min(block_size, bytes.size() - offset);
        impl_->writeExact(bytes.data() + offset, size, impl_->dataOffset(block));
        offset += size;
    }
    if (replacing) {
        transaction.commit({{id, inode}});
    } else {
        transaction.commit({{id, inode}, {parent.id, parent.inode}});
        impl_->inode_hint = id + 1;
    }
}

Bytes Disk::read(const std::string& path) const {
    std::lock_guard lock(impl_->mutex);
    const auto id = impl_->lookup(splitPath(path));
    const auto& inode = impl_->inodes[id];
    if (inode.kind == InodeKind::directory) {
        fail(EISDIR, "read directory as a file");
    }
    Bytes bytes(impl_->checkedSize(inode));
    impl_->readData(inode, bytes.data(), bytes.size());
    return bytes;
}

bool Disk::exists(const std::string& path) const {
    std::lock_guard lock(impl_->mutex);
    const auto components = splitPath(path);
    try {
        return impl_->resolve(components, components.size()).has_value();
    } catch (const std::system_error& error) {
        if (error.code() == std::errc::not_a_directory) {
            return false;
        }
        throw;
    }
}

std::vector<std::string> Disk::list(const std::string& path) const {
    std::lock_guard lock(impl_->mutex);
    const auto id = impl_->lookup(splitPath(path));
    const auto entries = impl_->readDirectory(impl_->inodes[id]);
    std::vector<std::string> names;
    names.reserve(entries.size());
    for (const auto& entry : entries) {
        names.emplace_back(entry.name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

void Disk::remove(const std::string& path) {
    std::lock_guard lock(impl_->mutex);
    const auto components = splitPath(path);
    if (components.empty()) {
        fail(EBUSY, "remove root directory");
    }
    auto parent = impl_->parentOf(components);
    const auto entry = findEntry(parent.entries, components.back());
    if (entry == parent.entries.end()) {
        fail(ENOENT, "path does not exist");
    }
    const auto id = entry->inode;
    const auto& inode = impl_->inodes[id];
    if (inode.kind == InodeKind::directory && inode.size != 0) {
        fail(ENOTEMPTY, "remove nonempty directory");
    }
    Impl::Transaction transaction(*impl_);
    transaction.eraseEntry(parent.inode, parent.entries,
                           static_cast<std::size_t>(entry - parent.entries.cbegin()));
    transaction.releaseFile(inode);
    transaction.commit({{parent.id, parent.inode}, {id, Inode{}}});
    impl_->inode_hint = std::min(impl_->inode_hint, id);
}

DiskStats Disk::stats() const {
    std::lock_guard lock(impl_->mutex);
    std::size_t free_blocks = 0;
    for (Id id = impl_->super.data_start; id < impl_->super.block_count; ++id) {
        free_blocks += !allocated(impl_->bitmap, id);
    }
    const auto free_inodes = std::count_if(impl_->inodes.begin(), impl_->inodes.end(),
                                         [](const Inode& inode) { return inode.kind == InodeKind::free; });
    return {impl_->super.block_count - impl_->super.data_start, free_blocks,
            impl_->inodes.size(), static_cast<std::size_t>(free_inodes)};
}

} // namespace tileflow
