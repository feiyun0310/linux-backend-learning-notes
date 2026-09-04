#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

enum class LogLevel {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
};

static const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "UNKNOWN";
}

class RollingFile {
public:
    RollingFile(std::string path, std::size_t max_bytes, int max_backups)
        : path_(std::move(path)),
          max_bytes_(max_bytes),
          max_backups_(max_backups) {
        std::filesystem::path path(path_);
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }
        open_append();
    }

    void write_line(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_size_ + line.size() + 1 > max_bytes_) {
            roll();
        }
        stream_ << line << '\n';
        stream_.flush();
        current_size_ += line.size() + 1;
    }

private:
    void open_append() {
        stream_.open(path_, std::ios::app);
        if (!stream_) {
            throw std::runtime_error("cannot open log file: " + path_);
        }
        std::error_code error;
        current_size_ = std::filesystem::file_size(path_, error);
        if (error) {
            current_size_ = 0;
        }
    }

    void roll() {
        stream_.close();

        if (max_backups_ <= 0) {
            std::error_code error;
            std::filesystem::remove(path_, error);
        } else {
            for (int index = max_backups_ - 1; index >= 1; --index) {
                const std::string from = path_ + "." + std::to_string(index);
                const std::string to = path_ + "." + std::to_string(index + 1);
                std::error_code error;
                std::filesystem::remove(to, error);
                error.clear();
                if (std::filesystem::exists(from)) {
                    std::filesystem::rename(from, to, error);
                }
            }

            const std::string first_backup = path_ + ".1";
            std::error_code error;
            std::filesystem::remove(first_backup, error);
            error.clear();
            if (std::filesystem::exists(path_)) {
                std::filesystem::rename(path_, first_backup, error);
            }
        }

        stream_.open(path_, std::ios::out | std::ios::trunc);
        if (!stream_) {
            throw std::runtime_error("cannot create log file: " + path_);
        }
        current_size_ = 0;
    }

    std::string path_;
    std::size_t max_bytes_;
    int max_backups_;
    std::size_t current_size_ = 0;
    std::ofstream stream_;
    std::mutex mutex_;
};

class AsyncLogger {
public:
    static AsyncLogger& instance() {
        static AsyncLogger logger;
        return logger;
    }

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    void set_level(LogLevel level) {
        std::lock_guard<std::mutex> lock(config_mutex_);
        default_level_ = level;
    }

    void set_category_level(const std::string& category, LogLevel level) {
        std::lock_guard<std::mutex> lock(config_mutex_);
        category_levels_[category] = level;
    }

    void set_rolling_file(const std::string& path,
                          std::size_t max_bytes,
                          int max_backups) {
        auto file = std::make_shared<RollingFile>(path, max_bytes, max_backups);
        std::lock_guard<std::mutex> lock(config_mutex_);
        file_ = std::move(file);
    }

    void log(LogLevel level,
             const std::string& category,
             const std::string& message,
             const char* file,
             int line,
             const char* function) {
        std::shared_ptr<RollingFile> output;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            auto found = category_levels_.find(category);
            const LogLevel minimum =
                found == category_levels_.end() ? default_level_ : found->second;
            if (level < minimum) {
                return;
            }
            output = file_;
        }

        const std::string formatted =
            format(level, category, message, file, line, function);

        {
            std::lock_guard<std::mutex> lock(console_mutex_);
            std::cout << formatted << std::endl;
        }

        if (!output) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (queue_.size() >= max_queue_size_) {
                ++dropped_;
                return;
            }
            queue_.push_back({std::move(output), formatted});
        }
        queue_changed_.notify_one();
    }

    std::size_t dropped_count() const {
        return dropped_.load();
    }

private:
    struct PendingLine {
        std::shared_ptr<RollingFile> output;
        std::string text;
    };

    AsyncLogger() : worker_(&AsyncLogger::flush_loop, this) {}

    ~AsyncLogger() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stopping_ = true;
        }
        queue_changed_.notify_all();
        worker_.join();
    }

    static std::string format(LogLevel level,
                              const std::string& category,
                              const std::string& message,
                              const char* file,
                              int line,
                              const char* function) {
        const auto now = std::chrono::system_clock::now();
        const std::time_t timestamp = std::chrono::system_clock::to_time_t(now);
        std::tm local_time{};
        localtime_r(&timestamp, &local_time);

        const char* filename = std::strrchr(file, '/');
        filename = filename ? filename + 1 : file;

        std::ostringstream output;
        output << std::put_time(&local_time, "[%Y-%m-%d %H:%M:%S]")
               << " [" << level_name(level) << "]"
               << " [" << category << "]"
               << " [" << filename << ':' << line << "]"
               << " [" << function << "] "
               << message;
        return output.str();
    }

    void flush_loop() {
        for (;;) {
            std::deque<PendingLine> batch;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_changed_.wait(lock, [this] {
                    return stopping_ || !queue_.empty();
                });
                if (stopping_ && queue_.empty()) {
                    return;
                }
                batch.swap(queue_);
            }

            for (const PendingLine& line : batch) {
                line.output->write_line(line.text);
            }
        }
    }

    static constexpr std::size_t max_queue_size_ = 4096;
    LogLevel default_level_ = LogLevel::Info;
    std::map<std::string, LogLevel> category_levels_;
    std::shared_ptr<RollingFile> file_;
    std::mutex config_mutex_;
    std::mutex console_mutex_;

    std::deque<PendingLine> queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_changed_;
    bool stopping_ = false;
    std::thread worker_;
    std::atomic<std::size_t> dropped_{0};
};

#define LOG_CATEGORY(category, level, message) \
    AsyncLogger::instance().log(               \
        level, category, message, __FILE__, __LINE__, __func__)

#define LOG_INFO(message) \
    LOG_CATEGORY("default", LogLevel::Info, message)
#define LOG_WARN(message) \
    LOG_CATEGORY("default", LogLevel::Warn, message)
#define LOG_ERROR(message) \
    LOG_CATEGORY("default", LogLevel::Error, message)

int main() {
    AsyncLogger& logger = AsyncLogger::instance();
    logger.set_level(LogLevel::Info);
    logger.set_category_level("network", LogLevel::Debug);
    logger.set_rolling_file("./log/app.log", 64 * 1024, 3);

    LOG_INFO("application started");

    std::vector<std::thread> workers;
    for (int worker_id = 0; worker_id < 4; ++worker_id) {
        workers.emplace_back([worker_id] {
            for (int index = 0; index < 100; ++index) {
                LOG_CATEGORY(
                    "network",
                    LogLevel::Info,
                    "worker=" + std::to_string(worker_id) +
                        ", message=" + std::to_string(index));
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    LOG_WARN("all worker threads joined");
    std::cout << "dropped logs: " << logger.dropped_count() << std::endl;
    return 0;
}
