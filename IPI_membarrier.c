/*
 * Based on code from Strickler and Doty, modified for this project.
 * Original implementation: https://github.com/ColeStrickler/EECS750-FinalProject
 */
#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <syscall.h>
#include <sched.h>
#include <pthread.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <linux/membarrier.h>

#define VICTIM_CPU 0

int* thread_args;
int NUM_THREADS;
pthread_t* ipi_storm_threads;
unsigned long long membarrier_counter = 0;  // Added counter

inline static int membarrier(int cmd, unsigned int flags, int cpu_id)
{
    __sync_fetch_and_add(&membarrier_counter, 1);  // Increment counter
    return syscall(__NR_membarrier, cmd, flags, cpu_id);
}

void ipi_register(unsigned long num_threads)
{
    int ret = membarrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ, 0, 0);
    if (ret == -1) {
        printf("Error %d\n", errno);
        exit(EXIT_FAILURE);
    }
    ret = membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ, 0, 0);
    if (ret == -1) {
        printf("Error %d\n", errno);
        exit(EXIT_FAILURE);
    }
    NUM_THREADS = num_threads;
    thread_args = malloc(sizeof(int)*(NUM_THREADS));
    ipi_storm_threads = malloc(sizeof(pthread_t*)*(NUM_THREADS));
    for (int i = 2; i < NUM_THREADS+2; i++)
    {
        thread_args[i-2] = i;
    }
}

void pin_cpu(int cpu)
{
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &mask) == -1) {
        perror("sched_setaffinity");
        exit(EXIT_FAILURE);
    }
}

void ipi_storm_thread(void *cpu)
{
    int cpu_num = *(int*)cpu;
    pin_cpu(cpu_num);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    while(1)
    {
        int ret = membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ, MEMBARRIER_CMD_FLAG_CPU, VICTIM_CPU);
        if (ret < 0)
        {
            perror("membarrier()");
        }
    }
}

void begin_ipi_storm()
{
    for (int i = 0; i < NUM_THREADS; i++)
    {
        pthread_create(&ipi_storm_threads[i], NULL, ipi_storm_thread, &thread_args[i]);
    }
}

int kill_ipi()
{
    int ret;
    for (int i = 0; i < NUM_THREADS; i++)
    {
        ret = pthread_cancel(ipi_storm_threads[i]);
    }
    return ret;
}

int main(int argc, char *argv[]) {
    char command[32];
    int num_threads;
    int running = 0;
    int duration = 10;
    int target_cpu = 0;
    int num_cpus = 23;

    if (argc >= 2) {
        num_threads = atoi(argv[1]);
        if (num_threads <= 0) {
            fprintf(stderr, "Error: NUM_THREADS must be positive\n");
            return EXIT_FAILURE;
        }
    }

    if (argc >= 3) {
        duration = atoi(argv[2]);
        if (duration <= 0) {
            fprintf(stderr, "Error: TEST_DURATION must be positive\n");
            return EXIT_FAILURE;
        }
    }

    if (argc >= 4) {
        target_cpu = atoi(argv[3]);
        if (target_cpu < 0 || target_cpu >= num_cpus) {
            fprintf(stderr, "Error: TARGET_CPU must be between 0 and %d\n", num_cpus - 1);
            return EXIT_FAILURE;
        }
    }

    ipi_register(num_threads);
    begin_ipi_storm();
    sleep(duration);
    kill_ipi();

    printf("Total membarriers executed: %llu\n", membarrier_counter);  // Print counter

    return 0;
}