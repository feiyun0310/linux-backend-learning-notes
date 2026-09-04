#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <queue>
#include <random>
#include <thread>

class BlockingQueue {
public:
    explicit BlockingQueue(std::size_t capacity) : capacity_(capacity) {}

    bool TryPush(int value, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        bool ready = not_full_.wait_for(lock, timeout, [this] {
            return closed_ || queue_.size() < capacity_;
        });

        if (!ready || closed_) {
            return false;
        }

        queue_.push(value);
        not_empty_.notify_one();
        return true;
    }

    bool TryPop(int &value, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        bool ready = not_empty_.wait_for(lock, timeout, [this] {
            return closed_ || !queue_.empty();
        });

        if (!ready || queue_.empty()) {
            return false;
        }

        value = queue_.front();
        queue_.pop();
        not_full_.notify_one();
        return true;
    }

    void Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        not_full_.notify_all();
        not_empty_.notify_all();
    }

    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    bool ClosedAndEmpty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_ && queue_.empty();
    }

private:
    std::queue<int> queue_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    bool closed_ = false;
};

std::atomic<int> total_produced{0};
std::atomic<int> total_consumed{0};
std::mutex output_mutex;

void Producer(BlockingQueue &queue, char id, int min_value, int max_value,
              int min_delay_ms, int max_delay_ms) {
    std::random_device device;
    std::mt19937 generator(device());
    std::uniform_int_distribution<int> value_distribution(min_value, max_value);
    std::uniform_int_distribution<int> delay_distribution(min_delay_ms,
                                                           max_delay_ms);

    for (int i = 0; i < 50; ++i) {
        int value = value_distribution(generator);
        while (!queue.TryPush(value, std::chrono::milliseconds(500))) {
        }
        ++total_produced;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(delay_distribution(generator)));
    }

    std::lock_guard<std::mutex> lock(output_mutex);
    std::cout << "[Producer " << id << "] done\n";
}

void Consumer(BlockingQueue &queue, char id, int delay_ms) {
    while (true) {
        int value = 0;
        if (queue.TryPop(value, std::chrono::milliseconds(100))) {
            ++total_consumed;
            std::lock_guard<std::mutex> lock(output_mutex);
            std::cout << "[Consumer " << id << "] consumed " << value
                      << ", buffer: " << queue.Size() << '\n';
        } else if (queue.ClosedAndEmpty()) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }

    std::lock_guard<std::mutex> lock(output_mutex);
    std::cout << "[Consumer " << id << "] done\n";
}

void Statistics(BlockingQueue &queue) {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        int produced = total_produced.load();
        int consumed = total_consumed.load();
        std::size_t size = queue.Size();
        {
            std::lock_guard<std::mutex> lock(output_mutex);
            std::cout << "[Stats] produced=" << produced
                      << ", consumed=" << consumed
                      << ", buffer=" << size << '\n';
        }

        if (queue.ClosedAndEmpty() && consumed == produced) {
            break;
        }
    }
}

int main() {
    BlockingQueue queue(20);

    std::thread producer_a(Producer, std::ref(queue), 'A', 1, 100, 100, 300);
    std::thread producer_b(Producer, std::ref(queue), 'B', 101, 200, 50, 150);
    std::thread consumer_x(Consumer, std::ref(queue), 'X', 200);
    std::thread consumer_y(Consumer, std::ref(queue), 'Y', 100);
    std::thread consumer_z(Consumer, std::ref(queue), 'Z', 300);
    std::thread statistics(Statistics, std::ref(queue));

    producer_a.join();
    producer_b.join();
    queue.Close();

    consumer_x.join();
    consumer_y.join();
    consumer_z.join();
    statistics.join();

    std::cout << "All done: produced=" << total_produced.load()
              << ", consumed=" << total_consumed.load() << '\n';
    return 0;
}
