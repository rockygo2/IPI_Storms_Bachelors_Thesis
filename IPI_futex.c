#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <linux/futex.h>
#include <limits.h>

#define DEFAULT_TEST_DURATION 10
#define DEFAULT_num_cpu 4
#define DEFAULT_TARGET_CPU 0

volatile int running = 1;
volatile unsigned long long wake_count = 0;
int num_cpus = 0;
volatile int *futex_variables = NULL;

void sigint_handler(int sig) {
    running = 0;
    printf("\nReceived interrupt, stopping test...\n");
}

static long futex(int *uaddr, int futex_op, int val,
                  const struct timespec *timeout, int *uaddr2, int val3) {
    return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

void *futex_waiter(void *arg) {
    int *thread_args = (int *)arg;
    int pair_id = thread_args[0];
    int target_cpu = thread_args[1];
    int *futex_addr = (int*)&futex_variables[pair_id];
    cpu_set_t cpuset;
    struct timespec timeout;
    int ret;
    
    CPU_ZERO(&cpuset);
    CPU_SET(target_cpu, &cpuset);
    
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) == -1) {
        perror("Failed to set CPU affinity for waiter");
        return NULL;
    }
    
    printf("Futex waiter %d running on CPU %d\n", pair_id, target_cpu);
    
    timeout.tv_sec = 0;
    timeout.tv_nsec = 1000000;
    
    *futex_addr = 0;
    
    while (running) {
        ret = futex(futex_addr, FUTEX_WAIT, 0, &timeout, NULL, 0);
        
        if (ret == -1 && errno != EAGAIN && errno != ETIMEDOUT) {
            perror("futex wait");
        }
        
        sched_yield();
    }
    
    return NULL;
}


void *futex_waker(void *arg) {
    int *thread_args = (int *)arg;
    int pair_id = thread_args[0];
    int target_cpu = thread_args[1];
    int *futex_addr = (int*)&futex_variables[pair_id];
    int source_cpu = 1 + (pair_id % (num_cpus - 1)); // Use any CPU except target
    
    if (source_cpu == target_cpu) {
        source_cpu = (source_cpu + 1) % num_cpus;
    }
    
    cpu_set_t cpuset;
    int ret;
    
    CPU_ZERO(&cpuset);
    CPU_SET(source_cpu, &cpuset);
    
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) == -1) {
        perror("Failed to set CPU affinity for waker");
        return NULL;
    }
    
    printf("Futex waker %d running on CPU %d, targeting futex on CPU %d\n", 
           pair_id, source_cpu, target_cpu);
    
    while (running) {
        __sync_fetch_and_add(futex_addr, 1);
        
        ret = futex(futex_addr, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
        if (ret == -1) {
            perror("futex wake");
        }
        
        wake_count++;
    }
    
    return NULL;
}

void run_test(int duration, int num_cpu, int target_cpu) {
    pthread_t *waiter_threads = malloc(num_cpu * sizeof(pthread_t));
    pthread_t *waker_threads = malloc(num_cpu * sizeof(pthread_t));
    int (*thread_args)[2] = malloc(num_cpu * sizeof(*thread_args));
    int i;
    
    if (!waiter_threads || !waker_threads || !thread_args) {
        perror("Failed to allocate memory for threads");
        exit(EXIT_FAILURE);
    }
    
    wake_count = 0;
    running = 1;
    
    printf("Starting futex(FUTEX_WAKE) IPI storm test targeting CPU %d for %d seconds with %d pairs...\n", 
           target_cpu, duration, num_cpu);
    
    for (i = 0; i < num_cpu; i++) {
        thread_args[i][0] = i;           
        thread_args[i][1] = target_cpu;
    }

    for (i = 0; i < num_cpu; i++) {
        if (pthread_create(&waiter_threads[i], NULL, futex_waiter, thread_args[i])) {
            perror("Failed to create waiter thread");
            exit(EXIT_FAILURE);
        }
    }
    
    usleep(100000);
    
    for (i = 0; i < num_cpu; i++) {
        if (pthread_create(&waker_threads[i], NULL, futex_waker, thread_args[i])) {
            perror("Failed to create waker thread");
            exit(EXIT_FAILURE);
        }
    }
    
    sleep(duration);
    running = 0;
    
    for (i = 0; i < num_cpu; i++) {
        pthread_join(waiter_threads[i], NULL);
        pthread_join(waker_threads[i], NULL);
    }
    
    printf("\nTest completed.\n");
    printf("Futex wake operations: %llu\n", wake_count);
    printf("Futex wake operations per second: %llu\n", wake_count / duration);
    printf("--------------------------------------------\n");
    
    free(waiter_threads);
    free(waker_threads);
    free(thread_args);
}

void print_usage(const char *program_name) {
    printf("Usage: %s [NUM_CPU] [TEST_DURATION] [TARGET_CPU]\n", program_name);
    printf("  num_cpu  Number of futex pairs (default: %d)\n", DEFAULT_num_cpu);
    printf("  TEST_DURATION    Test duration in seconds (default: %d)\n", DEFAULT_TEST_DURATION);
    printf("  TARGET_CPU       Target CPU to hammer with IPIs (default: %d)\n", DEFAULT_TARGET_CPU);
}

int main(int argc, char *argv[]) {
    int duration = DEFAULT_TEST_DURATION;
    int num_cpu = DEFAULT_num_cpu;
    int target_cpu = DEFAULT_TARGET_CPU;
    int num_iterations = 1;
    int i;
    struct sigaction sa;
    
    num_cpus = sysconf(_SC_NPROCESSORS_ONLN);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);
    
    // Parse command line arguments
    if (argc >= 2) {
        num_cpu = atoi(argv[1]);
        if (num_cpu <= 0) {
            fprintf(stderr, "Error: num_cpu must be positive\n");
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    
    if (argc >= 3) {
        duration = atoi(argv[2]);
        if (duration <= 0) {
            fprintf(stderr, "Error: TEST_DURATION must be positive\n");
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    
    if (argc >= 4) {
        target_cpu = atoi(argv[3]);
        if (target_cpu < 0 || target_cpu >= num_cpus) {
            fprintf(stderr, "Error: TARGET_CPU must be between 0 and %d\n", num_cpus - 1);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    
    // This was for testing purposes
    if (argc >= 5) {
        num_iterations = atoi(argv[4]);
        if (num_iterations <= 0) {
            fprintf(stderr, "Error: Number of iterations must be positive\n");
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    
    futex_variables = mmap(NULL, num_cpu * sizeof(int), 
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (futex_variables == MAP_FAILED) {
        perror("mmap");
        exit(EXIT_FAILURE);
    }
    
    printf("Futex WAKE IPI Storm Test Configuration:\n");
    printf("- Available CPUs: %d\n", num_cpus);
    printf("- Target CPU: %d\n", target_cpu);
    printf("- Test duration: %d seconds\n", duration);
    printf("- Number of test iterations: %d\n", num_iterations);
    printf("- Number of futex pairs: %d\n", num_cpu);
    printf("--------------------------------------------\n");
    
    for (i = 0; i < num_iterations; i++) {
        if (num_iterations > 1) {
            printf("\nIteration %d of %d\n", i+1, num_iterations);
        }
        run_test(duration, num_cpu, target_cpu);
    
    }
    
    munmap((void*)futex_variables, num_cpu * sizeof(int));
    
    return 0;
}