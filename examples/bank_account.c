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
