#include "service.hpp"
#include "tileflow/disk.hpp"
#include "tileflow/worker.hpp"
#include <cpp-httplib/httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>

#include <poll.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <unistd.h>

namespace tileflow {
namespace {

using Json = nlohmann::json;
using Id = std::uint64_t;
using namespace std::chrono_literals;
constexpr std::size_t max_upload = 24UL * 1024 * 1024;
volatile std::sig_atomic_t stopping = 0;

void stop_signal(int) { stopping = 1; }

auto timestamp() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string serialize(const Json& value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

Bytes bytes(const std::string& text) { return Bytes(text.begin(), text.end()); }
std::string base(Id id) { return "/jobs/" + std::to_string(id); }
std::string tile_path(Id id, std::size_t index) {
    return base(id) + "/tiles/" + std::to_string(index) + ".png";
}

struct HttpError : std::runtime_error {
    int code;
    HttpError(int code, const std::string& message) : std::runtime_error(message), code(code) {}
};

struct Account { std::string username, role; };
struct Session { Account account; std::int64_t expires; };

std::string cookie_token(const httplib::Request& request) {
    const auto cookie = request.get_header_value("Cookie");
    const std::string key = "tileflow_session=";
    std::size_t begin = 0;
    while (begin < cookie.size()) {
        begin = cookie.find_first_not_of(' ', begin);
        if (begin == std::string::npos) break;
        const auto end = cookie.find(';', begin);
        if (cookie.compare(begin, key.size(), key) == 0)
            return cookie.substr(begin + key.size(), end == std::string::npos ? end : end - begin - key.size());
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return {};
}

std::string new_token() {
    std::array<unsigned char, 24> data{};
    for (std::size_t current = 0; current < data.size();) {
        const auto count = getrandom(data.data() + current, data.size() - current, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("Session token generation failed");
        current += static_cast<std::size_t>(count);
    }
    std::ostringstream out;
    for (auto byte : data) out << std::hex << std::setw(2) << std::setfill('0') << int(byte);
    return out.str();
}

struct Tile {
    Rect rect;
    std::string status = "queued";
    pid_t pid = 0;
    Json attempts = Json::array();
};

struct Job {
    Id id = 0;
    std::string owner, name, op, priority, error;
    std::string status = "queued";
    std::int64_t submitted = 0, finished = 0;
    int width = 0, height = 0;
    std::size_t completed = 0;
    Image image;
    std::vector<Tile> tiles;
};

struct Item { Id job; std::size_t tile; Id order; };
struct Assignment { Item item; Id attempt; };
struct Slot {
    pid_t pid = 0;
    int fd = -1;
    std::optional<Assignment> work;
    std::string fault;
};

Json describe(const Job& job) {
    Json tiles = Json::array();
    for (std::size_t i = 0; i < job.tiles.size(); ++i) {
        const auto& tile = job.tiles[i];
        tiles.push_back({{"id", i}, {"x", tile.rect.x}, {"y", tile.rect.y}, {"w", tile.rect.w},
                         {"h", tile.rect.h}, {"status", tile.status}, {"pid", tile.pid},
                         {"attempts", tile.attempts}});
    }
    return {{"id", job.id}, {"owner", job.owner}, {"name", job.name}, {"op", job.op},
            {"priority", job.priority}, {"status", job.status}, {"width", job.width}, {"height", job.height},
            {"done", job.completed}, {"total", job.tiles.size()}, {"error", job.error},
            {"submitted", job.submitted}, {"finished", job.finished}, {"tiles", std::move(tiles)}};
}

class Scheduler {
public:
    explicit Scheduler(Config options) :
        options_(std::move(options)), disk_(options_.disk, std::size_t(options_.disk_mib) * 1024 * 1024),
        slots_(options_.workers) {
        disk_.mkdir("/jobs");
        restore();
    }

    ~Scheduler() { shutdown(); }

    void start() {
        std::lock_guard lock(mutex_);
        for (auto& slot : slots_) spawn(slot);
        thread_ = std::thread([this] { run(); });
    }

    void shutdown() {
        {
            std::lock_guard lock(mutex_);
            if (closing_) return;
            closing_ = true;
        }
        if (thread_.joinable()) thread_.join();
        for (auto& slot : slots_) {
            if (slot.pid > 0) kill(slot.pid, SIGKILL);
            if (slot.fd >= 0) close(slot.fd);
            if (slot.pid > 0) {
                while (waitpid(slot.pid, nullptr, 0) < 0 && errno == EINTR) {}
            }
            slot = Slot{};
        }
        for (const auto& entry : jobs_) {
            if (active(entry.second)) checkpoint(entry.second);
        }
    }

    std::pair<Account, std::string> login(const std::string& name, const std::string& password) {
        const char* key = nullptr;
        std::string role = "user";
        if (name == "admin") { key = "TILEFLOW_ADMIN_PASSWORD"; role = "admin"; }
        else if (name == "alice") key = "TILEFLOW_ALICE_PASSWORD";
        else if (name == "bob") key = "TILEFLOW_BOB_PASSWORD";
        const char* configured = key ? std::getenv(key) : nullptr;
        if (!key || password != (configured ? std::string(configured) : name + "-demo"))
            throw HttpError(401, "用户名或密码不正确");
        auto token = new_token();
        Account account{name, role};
        std::lock_guard lock(mutex_);
        const auto now = timestamp();
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (it->second.expires <= now) it = sessions_.erase(it);
            else ++it;
        }
        if (sessions_.size() >= 256) throw HttpError(503, "会话数量已达上限");
        sessions_.emplace(token, Session{account, now + 8 * 60 * 60 * 1000});
        return {account, token};
    }

    Account identify(const httplib::Request& request) {
        std::lock_guard lock(mutex_);
        const auto it = sessions_.find(cookie_token(request));
        if (it == sessions_.end() || it->second.expires <= timestamp()) throw HttpError(401, "请先登录");
        return it->second.account;
    }

    void logout(const httplib::Request& request) {
        std::lock_guard lock(mutex_);
        sessions_.erase(cookie_token(request));
    }

    Id submit(const Account& user, const std::string& name, std::string op, std::string priority,
              const Bytes& input) {
        if (!supported_operation(op) || (priority != "high" && priority != "normal"))
            throw HttpError(400, "请选择有效的操作和优先级");
        const bool jpeg = input.size() >= 3 && input[0] == 0xff && input[1] == 0xd8 && input[2] == 0xff;
        const std::array<std::uint8_t, 8> png{0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
        if (!jpeg && (input.size() < png.size() || !std::equal(png.begin(), png.end(), input.begin())))
            throw HttpError(415, "请选择有效的 JPEG 或 PNG 图像");
        if (input.size() > max_upload) throw HttpError(413, "图像文件不能超过 24 MiB");
        Image source;
        try { source = decode(input); }
        catch (const std::exception&) { throw HttpError(400, "图像无法解码或尺寸超过限制"); }

        std::lock_guard lock(mutex_);
        if (std::count_if(jobs_.begin(), jobs_.end(), [](const auto& entry) { return active(entry.second); }) >= 8)
            throw HttpError(429, "已有八个未完成作业，请稍后再提交");
        Job job;
        job.id = next_job_++;
        job.owner = user.username;
        job.name = name.empty() ? "image" : name.substr(0, 160);
        job.op = std::move(op);
        job.priority = std::move(priority);
        job.submitted = timestamp();
        job.width = source.width;
        job.height = source.height;
        job.image = std::move(source);
        for (const auto rect : split(job.width, job.height, options_.tile_size)) job.tiles.push_back(Tile{rect});
        const auto path = base(job.id);
        try {
            disk_.mkdir(path);
            disk_.mkdir(path + "/tiles");
            disk_.put(path + "/source", input);
            persist(job);
        } catch (const std::exception& e) {
            for (const auto& suffix : {"/meta.json", "/source", "/tiles", ""}) {
                try { if (disk_.exists(path + suffix)) disk_.remove(path + suffix); }
                catch (const std::exception&) {}
            }
            throw HttpError(507, std::string("镜像无法保存作业: ") + e.what());
        }
        const Id id = job.id;
        jobs_.emplace(id, std::move(job));
        const auto& saved = jobs_.at(id);
        event("submitted", id, 0, 0, 0, saved.priority == "high" ? "高优先级作业已入队" : "普通作业已入队");
        for (std::size_t i = 0; i < saved.tiles.size(); ++i) enqueue(id, i);
        return id;
    }

    Json state(const Account& user, Id since) {
        std::lock_guard lock(mutex_);
        Json jobs = Json::array(), workers = Json::array(), log = Json::array();
        const bool admin = user.role == "admin";
        for (auto it = jobs_.rbegin(); it != jobs_.rend(); ++it)
            if (admin || it->second.owner == user.username) jobs.push_back(describe(it->second));
        for (const auto& item : events_) {
            if (item.at("seq").get<Id>() <= since) continue;
            const auto id = item.at("job").get<Id>();
            if (admin || (id && jobs_.at(id).owner == user.username)) log.push_back(item);
        }
        Json queues = {{"high", Json::array()}, {"normal", Json::array()}};
        Json storage = nullptr;
        if (admin) {
            for (std::size_t index = 0; index < slots_.size(); ++index) {
                const auto& slot = slots_[index];
                workers.push_back({{"slot", index + 1}, {"pid", slot.pid},
                                   {"job", slot.work ? Json(slot.work->item.job) : Json(nullptr)},
                                   {"tile", slot.work ? Json(slot.work->item.tile) : Json(nullptr)}});
            }
            for (const auto& item : high_) queues["high"].push_back({{"job", item.job}, {"tile", item.tile}});
            for (const auto& item : normal_) queues["normal"].push_back({{"job", item.job}, {"tile", item.tile}});
            const auto st = disk_.stats();
            storage = {{"total_blocks", st.total_blocks}, {"free_blocks", st.free_blocks},
                       {"total_inodes", st.total_inodes}, {"free_inodes", st.free_inodes}};
        }
        return {{"user", {{"username", user.username}, {"role", user.role}}}, {"jobs", std::move(jobs)},
                {"workers", std::move(workers)}, {"queues", std::move(queues)}, {"disk", std::move(storage)},
                {"events", std::move(log)}, {"last_seq", next_event_ - 1},
                {"policy", options_.fifo ? "fifo" : "priority"}, {"demo_delay_ms", options_.delay_ms}};
    }

    Bytes file(const Account& user, Id id, const std::string& kind, std::size_t index = 0) {
        std::lock_guard lock(mutex_);
        const auto it = jobs_.find(id);
        if (it == jobs_.end() || (user.role != "admin" && it->second.owner != user.username))
            throw HttpError(404, "找不到作业");
        const auto& job = it->second;
        if (kind == "source") return disk_.read(base(id) + "/source");
        if (kind == "result" && job.status == "done") return disk_.read(base(id) + "/final.png");
        if (kind == "tile" && index < job.tiles.size() && job.tiles[index].status == "done")
            return disk_.read(tile_path(id, index));
        throw HttpError(404, "图像尚不可用");
    }

    void terminate(const Account& user, pid_t pid) {
        if (user.role != "admin") throw HttpError(403, "需要管理员权限");
        std::lock_guard lock(mutex_);
        for (auto& slot : slots_) {
            if (pid <= 0 || slot.pid != pid || !slot.work || !slot.fault.empty()) continue;
            if (kill(pid, SIGKILL) < 0) break;
            slot.fault = "管理员已终止进程 (SIGKILL)";
            return;
        }
        throw HttpError(409, "该进程已退出或当前没有执行中的图块");
    }

private:
    static bool active(const Job& job) { return job.status == "queued" || job.status == "running"; }

    void persist(const Job& job) {
        disk_.put(base(job.id) + "/meta.json", bytes(serialize(describe(job))));
    }

    void checkpoint(const Job& job) {
        try { persist(job); }
        catch (const std::exception& e) { std::cerr << "Job " << job.id << " metadata: " << e.what() << '\n'; }
    }

    void restore() {
        for (const auto& folder : disk_.list("/jobs")) {
            if (folder.empty() || folder.find_first_not_of("0123456789") != std::string::npos) continue;
            const auto id = static_cast<Id>(std::stoull(folder));
            next_job_ = std::max(next_job_, id + 1);
            if (!disk_.exists(base(id) + "/meta.json")) continue;
            const auto raw = disk_.read(base(id) + "/meta.json");
            const auto snapshot = Json::parse(raw);
            Job job;
            job.id = id;
            job.owner = snapshot.at("owner");
            job.name = snapshot.at("name");
            job.op = snapshot.at("op");
            job.priority = snapshot.at("priority");
            job.status = snapshot.at("status");
            job.error = snapshot.at("error");
            job.submitted = snapshot.at("submitted");
            job.finished = snapshot.at("finished");
            job.width = snapshot.at("width");
            job.height = snapshot.at("height");
            for (const auto& record : snapshot.at("tiles")) {
                Tile tile{{record.at("x"), record.at("y"), record.at("w"), record.at("h")}};
                tile.attempts = record.at("attempts");
                for (auto& attempt : tile.attempts) {
                    next_attempt_ = std::max(next_attempt_, attempt.at("attempt").get<Id>() + 1);
                    if (attempt.at("status") == "running") {
                        attempt["end"] = timestamp();
                        attempt["status"] = "failed";
                        attempt["error"] = "服务端已重新打开镜像";
                    }
                }
                if (disk_.exists(tile_path(id, job.tiles.size()))) {
                    tile.status = "done";
                    tile.pid = record.at("pid");
                    ++job.completed;
                } else if (job.status == "failed") {
                    tile.status = "failed";
                }
                job.tiles.push_back(std::move(tile));
            }
            if (disk_.exists(base(id) + "/final.png")) {
                job.status = "done";
                if (!job.finished) job.finished = timestamp();
            } else if (active(job)) {
                job.status = "queued";
                if (job.completed == job.tiles.size()) finalize(job);
                else job.image = decode(disk_.read(base(id) + "/source"));
            }
            jobs_.emplace(id, std::move(job));
        }
        for (const auto& [id, job] : jobs_) {
            if (!active(job)) continue;
            for (std::size_t i = 0; i < job.tiles.size(); ++i)
                if (job.tiles[i].status == "queued") enqueue(id, i);
        }
    }

    void enqueue(Id id, std::size_t i) {
        auto& queue = jobs_.at(id).priority == "high" ? high_ : normal_;
        queue.push_back({id, i, next_order_++});
    }

    std::optional<Item> next_item() {
        if (high_.empty() && normal_.empty()) return {};
        const bool normal_first = !normal_.empty() && (high_.empty() ||
            (options_.fifo && normal_.front().order < high_.front().order));
        auto& queue = normal_first ? normal_ : high_;
        const Item item = queue.front();
        queue.pop_front();
        return item;
    }

    void event(const char* type, Id job, std::size_t tile, pid_t pid, Id attempt, const std::string& message) {
        events_.push_back({{"seq", next_event_++}, {"time", timestamp()}, {"type", type}, {"job", job},
                           {"tile", tile}, {"pid", pid}, {"attempt", attempt}, {"message", message}});
        if (events_.size() > 4096) events_.pop_front();
    }

    void spawn(Slot& slot) {
        const auto child = spawn_worker(options_.executable);
        slot = Slot{child.pid, child.fd, {}, {}};
        event("worker_spawn", 0, 0, slot.pid, 0, "工作进程已启动");
    }

    void dispatch() {
        for (auto& slot : slots_) {
            if (slot.work || slot.pid <= 0 || !slot.fault.empty()) continue;
            const auto item = next_item();
            if (!item) break;
            auto& job = jobs_.at(item->job);
            auto& tile = job.tiles.at(item->tile);
            const Id attempt = next_attempt_++;
            slot.work = Assignment{*item, attempt};
            tile.status = "running";
            tile.pid = slot.pid;
            tile.attempts.push_back({{"attempt", attempt}, {"pid", slot.pid}, {"start", timestamp()},
                                     {"end", nullptr}, {"status", "running"}, {"error", ""}});
            job.status = "running";
            event("dispatch", job.id, item->tile, slot.pid, attempt, "已派发图块");
            if (!send_work(slot.fd, make_work(job.image, tile.rect, job.op, attempt, options_.delay_ms))) {
                slot.fault = "派发失败，工作进程通信已关闭";
                kill(slot.pid, SIGKILL);
            }
        }
    }

    void mark_failed(Job& job, const std::string& message) {
        job.status = "failed";
        job.finished = timestamp();
        job.error = message;
        job.image = Image{};
        const auto other_job = [&job](const Item& i) { return i.job == job.id; };
        high_.erase(std::remove_if(high_.begin(), high_.end(), other_job), high_.end());
        normal_.erase(std::remove_if(normal_.begin(), normal_.end(), other_job), normal_.end());
        for (auto& tile : job.tiles) if (tile.status == "queued") tile.status = "failed";
        event("job_failed", job.id, 0, 0, 0, message);
        checkpoint(job);
    }

    void finalize(Job& job) {
        try {
            Image full{job.width, job.height, Bytes(static_cast<std::size_t>(job.width) * job.height * 3)};
            for (std::size_t i = 0; i < job.tiles.size(); ++i)
                paste(full, job.tiles[i].rect, decode(disk_.read(tile_path(job.id, i))));
            disk_.put(base(job.id) + "/final.png", encode(full));
            job.status = "done";
            job.finished = timestamp();
            job.image = Image{};
            event("job_done", job.id, 0, 0, 0, "所有图块已提交，最终图像可下载");
            checkpoint(job);
        } catch (const std::exception& e) {
            mark_failed(job, std::string("保存最终图像失败: ") + e.what());
        }
    }

    void complete(Slot& slot, Result result) {
        if (!slot.work || result.attempt != slot.work->attempt) return;
        const auto assignment = *slot.work;
        slot.work.reset();
        auto& job = jobs_.at(assignment.item.job);
        auto& tile = job.tiles.at(assignment.item.tile);
        auto& trace = tile.attempts.back();
        trace["end"] = timestamp();
        try {
            if (!active(job)) throw std::runtime_error("作业已终止");
            if (!result.error.empty()) throw std::runtime_error(result.error);
            if (result.image.width != tile.rect.w || result.image.height != tile.rect.h ||
                result.image.rgb.size() != static_cast<std::size_t>(tile.rect.w) * tile.rect.h * 3)
                throw std::runtime_error("图块尺寸或字节数错误");
            disk_.put(tile_path(job.id, assignment.item.tile), encode(result.image));
            trace["status"] = "done";
            tile.status = "done";
            ++job.completed;
            event("commit", job.id, assignment.item.tile, slot.pid, assignment.attempt, "图块已写入磁盘镜像");
            if (job.completed == job.tiles.size()) finalize(job);
            else checkpoint(job);
        } catch (const std::exception& e) {
            tile.status = "failed";
            trace["status"] = "failed";
            trace["error"] = e.what();
            event("tile_failed", job.id, assignment.item.tile, slot.pid, assignment.attempt, e.what());
            if (active(job)) mark_failed(job, std::string("图块无法提交: ") + e.what());
            else checkpoint(job);
        }
    }

    void reap(Slot& slot) {
        if (slot.pid <= 0) return;
        int status = 0;
        const auto pid = waitpid(slot.pid, &status, WNOHANG);
        if (pid <= 0) return;
        const auto reason = slot.fault.empty()
            ? (WIFSIGNALED(status) ? "进程退出，信号 " + std::to_string(WTERMSIG(status))
                                   : "进程退出，状态 " + std::to_string(WEXITSTATUS(status)))
            : slot.fault;
        if (slot.work) {
            const auto assignment = *slot.work;
            auto& job = jobs_.at(assignment.item.job);
            auto& tile = job.tiles.at(assignment.item.tile);
            auto& trace = tile.attempts.back();
            trace["end"] = timestamp();
            trace["status"] = "failed";
            trace["error"] = reason;
            event("worker_exit", job.id, assignment.item.tile, pid, assignment.attempt, reason);
            tile.pid = 0;
            if (active(job)) {
                tile.status = "queued";
                enqueue(job.id, assignment.item.tile);
                event("requeue", job.id, assignment.item.tile, pid, assignment.attempt, "图块按原优先级重新入队");
            } else {
                tile.status = "failed";
            }
            checkpoint(job);
        } else {
            event("worker_exit", 0, 0, pid, 0, reason);
        }
        if (slot.fd >= 0) close(slot.fd);
        slot = Slot{};
    }

    void run() {
        std::vector<pollfd> fds(slots_.size());
        for (;;) {
            {
                std::lock_guard lock(mutex_);
                if (closing_) return;
                for (std::size_t i = 0; i < slots_.size(); ++i) fds[i] = {slots_[i].fd, POLLIN, 0};
            }
            poll(fds.data(), fds.size(), 15);
            for (std::size_t i = 0; i < slots_.size(); ++i) {
                if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                Result result{};
                const bool received = recv_result(fds[i].fd, result);
                std::lock_guard lock(mutex_);
                auto& slot = slots_[i];
                if (received) {
                    complete(slot, std::move(result));
                } else {
                    close(slot.fd);
                    slot.fd = -1;
                    kill(slot.pid, SIGKILL);
                }
            }
            std::lock_guard lock(mutex_);
            if (closing_) return;
            for (auto& slot : slots_) {
                // A final result can arrive between poll() and waitpid(). Drain that
                // frame first so the decision follows actual commit state.
                if (slot.fd >= 0 && slot.work) {
                    pollfd fd{slot.fd, POLLIN, 0};
                    if (poll(&fd, 1, 0) > 0 && (fd.revents & POLLIN)) continue;
                }
                reap(slot);
                if (slot.pid == 0) {
                    try { spawn(slot); }
                    catch (const std::exception& e) { std::cerr << "Worker replacement: " << e.what() << '\n'; }
                }
            }
            dispatch();
        }
    }

    Config options_;
    Disk disk_;
    std::vector<Slot> slots_;
    std::mutex mutex_;
    std::thread thread_;
    bool closing_ = false;
    std::map<Id, Job> jobs_;
    std::deque<Item> high_, normal_;
    std::deque<Json> events_;
    std::map<std::string, Session> sessions_;
    Id next_job_ = 1, next_event_ = 1, next_attempt_ = 1, next_order_ = 1;
};

void reply(httplib::Response& response, const Json& value, int code = 200) {
    response.status = code;
    response.set_header("Cache-Control", "no-store");
    response.set_content(serialize(value), "application/json; charset=utf-8");
}

template<class Fn>
auto endpoint(Fn handle) {
    return [handle = std::move(handle)](const httplib::Request& request, httplib::Response& response) {
        try { handle(request, response); }
        catch (const HttpError& e) { reply(response, {{"error", e.what()}}, e.code); }
        catch (const Json::exception&) { reply(response, {{"error", "请求格式无效"}}, 400); }
        catch (const std::out_of_range&) { reply(response, {{"error", "编号超出范围"}}, 400); }
        catch (const std::exception& e) {
            std::cerr << request.path << ": " << e.what() << '\n';
            reply(response, {{"error", "服务端无法完成请求"}}, 500);
        }
    };
}

void image_response(httplib::Response& response, const Bytes& data, const char* type) {
    response.set_header("Cache-Control", "private, no-store");
    response.set_content(reinterpret_cast<const char*>(data.data()), data.size(), type);
}

}

int serve(const Config& options) {
    Scheduler scheduler(options);
    httplib::Server http;
    http.set_payload_max_length(max_upload + 65536);
    http.set_read_timeout(15);
    http.set_write_timeout(15);
    http.set_keep_alive_timeout(3);
    if (!http.set_mount_point("/", options.web.string()))
        throw std::runtime_error("Cannot load web assets: " + options.web.string());
    http.Get("/api/health", [](const auto&, auto& response) { reply(response, {{"status", "ok"}}); });
    http.Post("/api/login", endpoint([&](const auto& request, auto& response) {
        const auto body = Json::parse(request.body);
        const auto [account, token] = scheduler.login(body.at("username"), body.at("password"));
        response.set_header("Set-Cookie", "tileflow_session=" + token +
            "; Path=/; HttpOnly; SameSite=Strict; Max-Age=28800");
        reply(response, {{"user", {{"username", account.username}, {"role", account.role}}}});
    }));
    http.Post("/api/logout", endpoint([&](const auto& request, auto& response) {
        scheduler.logout(request);
        response.set_header("Set-Cookie", "tileflow_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
        reply(response, {{"status", "ok"}});
    }));
    http.Get("/api/state", endpoint([&](const auto& request, auto& response) {
        const auto user = scheduler.identify(request);
        const Id since = request.has_param("since") ? std::stoull(request.get_param_value("since")) : 0;
        reply(response, scheduler.state(user, since));
    }));
    http.Post("/api/jobs", endpoint([&](const auto& request, auto& response) {
        const auto user = scheduler.identify(request);
        if (!request.form.has_file("image")) throw HttpError(400, "请上传图像");
        const auto& img = request.form.get_file("image");
        const auto id = scheduler.submit(user, img.filename, request.form.get_field("op"),
            request.form.get_field("priority"), bytes(img.content));
        reply(response, {{"id", id}}, 201);
    }));
    http.Get(R"(/api/jobs/(\d+)/(source|result))", endpoint([&](const auto& request, auto& response) {
        const auto user = scheduler.identify(request);
        const Id id = std::stoull(request.matches[1].str());
        const std::string kind = request.matches[2];
        const auto data = scheduler.file(user, id, kind);
        if (kind == "result") response.set_header("Content-Disposition",
            "attachment; filename=\"tileflow-" + std::to_string(id) + ".png\"");
        image_response(response, data, data.front() == 0xff ? "image/jpeg" : "image/png");
    }));
    http.Get(R"(/api/jobs/(\d+)/tiles/(\d+))", endpoint([&](const auto& request, auto& response) {
        const auto user = scheduler.identify(request);
        const Id id = std::stoull(request.matches[1].str());
        const auto index = std::stoull(request.matches[2].str());
        image_response(response, scheduler.file(user, id, "tile", index), "image/png");
    }));
    http.Post(R"(/api/workers/(\d+)/stop)", endpoint([&](const auto& request, auto& response) {
        const auto user = scheduler.identify(request);
        scheduler.terminate(user, std::stoi(request.matches[1].str()));
        reply(response, {{"status", "stopping"}}, 202);
    }));
    if (!http.bind_to_port("127.0.0.1", options.port))
        throw std::runtime_error("Cannot bind the selected local port");
    std::signal(SIGTERM, stop_signal);
    std::signal(SIGINT, stop_signal);
    scheduler.start();
    std::cout << "TileFlow OS listening on http://127.0.0.1:" << options.port
              << " (" << options.workers << " workers, " << (options.fifo ? "FIFO" : "priority") << ")\n"
              << std::flush;
    std::atomic<bool> stopped{false};
    std::thread listener([&] { http.listen_after_bind(); stopped = true; });
    while (!stopping && !stopped) std::this_thread::sleep_for(40ms);
    http.stop();
    listener.join();
    scheduler.shutdown();
    return 0;
}

}
