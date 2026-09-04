# 03 并发练习：银行账户与生产者消费者

## 银行账户练习

一个账户的 balance 是共享状态：

- 3 个存款线程，每个循环存款。
- 2 个取款线程，每个循环取款。
- 互斥锁保护余额的检查与修改。
- 余额不足时，取款线程使用条件变量等待。
- 存款完成后广播通知等待线程。

关键点是“检查余额”和“扣款”必须属于同一个临界区，否则两个取款线程可能同时认为余额足够。

正确的取款模型：

~~~text
加锁
while 余额不足:
    等待条件变量（等待时自动释放锁）
扣减余额
解锁
~~~

原练习代码值得修正的细节：

- 代码把 &deposit_ids[i] 传入线程，因此线程函数应使用 *(int*)arg 读取编号，不能直接把指针强转成整数。
- 使用 time(NULL) 时应包含 time.h。
- 随机数生成器本身也应考虑线程安全；工程代码可使用每线程随机引擎。

## 生产者消费者模型

生产者和消费者通过共享缓冲区解耦。它解决的不是“线程越多越快”，而是生产速度与消费速度不一致时的协调问题。

有界阻塞队列包含：

- 队列和容量上限。
- 一把保护队列的互斥锁。
- not_full：生产者等待队列非满。
- not_empty：消费者等待队列非空。
- TryPush(timeout) 和 TryPop(timeout)。

~~~mermaid
flowchart LR
    P1[Producer A] --> Q[Bounded Blocking Queue]
    P2[Producer B] --> Q
    Q --> C1[Consumer X]
    Q --> C2[Consumer Y]
    Q --> C3[Consumer Z]
    Q --> S[Statistics]
~~~

### 正确的等待关系

- 生产者：队列满时等待 not_full；放入数据后通知 not_empty。
- 消费者：队列空时等待 not_empty；取走数据后通知 not_full。

原思维导图中“消费者等待 not_full”是笔误，消费者应等待 not_empty。

## 停止语义

只统计“生产者完成数量”可以完成练习，但通用阻塞队列更适合提供 Close：

1. 生产者全部结束后关闭队列。
2. Close 唤醒所有等待线程。
3. 消费者继续取完剩余数据。
4. 队列关闭且为空时，消费者退出。

这样可以避免把“生产者数量”和“预计消息总数”硬编码进消费者。

## 验收测试

- 最终消费数量等于生产数量。
- 队列大小始终在 0..capacity 范围。
- 慢消费者不会导致无限占用内存。
- 超时不会破坏队列状态。
- 所有线程在停止后都能正常 join。
- 使用小容量、高并发和随机延迟重复压力测试。



## 完整可运行代码

下面的代码根据 PDF 原稿整理，保留原练习场景，并修复了线程参数、随机数、头文件和停止语义等问题。独立源文件也保存在仓库的 `examples/` 目录。

### 银行账户：pthread 互斥锁与条件变量

~~~c
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum {
    DEPOSIT_THREAD_COUNT = 3,
    WITHDRAW_THREAD_COUNT = 2,
    OPERATION_COUNT = 5
};

typedef struct {
    int id;
    unsigned int seed;
} WorkerArg;

static int balance = 1000;
static pthread_mutex_t balance_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t balance_changed = PTHREAD_COND_INITIALIZER;

static int random_range(unsigned int *seed, int min, int max) {
    return (int)(rand_r(seed) % (unsigned int)(max - min + 1)) + min;
}

static void sleep_ms(long milliseconds) {
    struct timespec duration = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L,
    };
    nanosleep(&duration, NULL);
}

static void *deposit_thread(void *arg) {
    WorkerArg *worker = arg;

    for (int i = 0; i < OPERATION_COUNT; ++i) {
        int amount = random_range(&worker->seed, 100, 500);

        pthread_mutex_lock(&balance_mutex);
        printf("[存款 %d] 准备存入 %d 元\n", worker->id, amount);
        balance += amount;
        printf("[存款 %d] 完成，余额 %d 元\n", worker->id, balance);
        pthread_cond_broadcast(&balance_changed);
        pthread_mutex_unlock(&balance_mutex);

        sleep_ms(100);
    }

    printf("[存款 %d] 完成全部操作\n", worker->id);
    return NULL;
}

static void *withdraw_thread(void *arg) {
    WorkerArg *worker = arg;

    for (int i = 0; i < OPERATION_COUNT; ++i) {
        int amount = random_range(&worker->seed, 50, 200);

        pthread_mutex_lock(&balance_mutex);
        printf("[取款 %d] 准备取出 %d 元\n", worker->id, amount);

        while (balance < amount) {
            printf("[取款 %d] 余额不足（%d < %d），等待存款\n",
                   worker->id, balance, amount);
            pthread_cond_wait(&balance_changed, &balance_mutex);
        }

        balance -= amount;
        printf("[取款 %d] 完成，余额 %d 元\n", worker->id, balance);
        pthread_mutex_unlock(&balance_mutex);

        sleep_ms(150);
    }

    printf("[取款 %d] 完成全部操作\n", worker->id);
    return NULL;
}

int main(void) {
    pthread_t deposit_threads[DEPOSIT_THREAD_COUNT];
    pthread_t withdraw_threads[WITHDRAW_THREAD_COUNT];
    WorkerArg deposit_args[DEPOSIT_THREAD_COUNT];
    WorkerArg withdraw_args[WITHDRAW_THREAD_COUNT];
    unsigned int base_seed = (unsigned int)time(NULL);

    printf("=== 银行账户系统启动 ===\n");
    printf("初始余额：%d 元\n\n", balance);

    for (int i = 0; i < DEPOSIT_THREAD_COUNT; ++i) {
        deposit_args[i] = (WorkerArg){
            .id = i + 1,
            .seed = base_seed ^ (unsigned int)(0x9e3779b9U * (i + 1)),
        };
        pthread_create(&deposit_threads[i], NULL, deposit_thread,
                       &deposit_args[i]);
    }

    for (int i = 0; i < WITHDRAW_THREAD_COUNT; ++i) {
        withdraw_args[i] = (WorkerArg){
            .id = i + 1,
            .seed = base_seed ^ (unsigned int)(0x85ebca6bU * (i + 1)),
        };
        pthread_create(&withdraw_threads[i], NULL, withdraw_thread,
                       &withdraw_args[i]);
    }

    for (int i = 0; i < DEPOSIT_THREAD_COUNT; ++i) {
        pthread_join(deposit_threads[i], NULL);
    }
    for (int i = 0; i < WITHDRAW_THREAD_COUNT; ++i) {
        pthread_join(withdraw_threads[i], NULL);
    }

    printf("\n=== 所有线程执行完毕 ===\n");
    printf("最终余额：%d 元\n", balance);

    pthread_cond_destroy(&balance_changed);
    pthread_mutex_destroy(&balance_mutex);
    return EXIT_SUCCESS;
}
~~~

编译与运行：

~~~bash
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -pthread \
  examples/bank_account.c -o bank_account
./bank_account
~~~

这个示例必须用 `while (balance < amount)` 重新检查条件，因为条件变量可能出现虚假唤醒，或者其他取款线程先一步改变余额。

### 生产者消费者：有界阻塞队列

~~~cpp
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
~~~

编译与运行：

~~~bash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread \
  examples/producer_consumer.cpp -o producer_consumer
./producer_consumer
~~~

该版本仍保留 PDF 中的超时等待、两个生产者、三个消费者和统计线程；同时加入 `Close()`，避免消费者依赖固定的生产者数量和消息总数才能退出。
