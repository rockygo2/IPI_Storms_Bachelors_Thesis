#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <signal.h>

#define PAGE_SIZE 4096

int NUM_THREADS = 22;
int DURATION_SEC = 5;
int VICTIM_CPU = 0;

volatile int running = 1;
volatile unsigned long long map_unmap_cycles = 0;
void *shared_region = NULL;

typedef struct {
    int num_cpus;
    unsigned long long *counts;
} tlb_counts_t;

tlb_counts_t get_tlb_counts(void) {
    tlb_counts_t result = {0, NULL};
    FILE *fp = fopen("/proc/interrupts", "r");
    if (!fp) {
        perror("Failed to open /proc/interrupts");
        return result;
    }

    char line[4096];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return result;
    }

    int num_cpus = 0;
    char *p = line;
    while ((p = strstr(p, "CPU"))) {
        num_cpus++;
        p++;
    }

    if (num_cpus == 0) {
        fprintf(stderr, "Error: Could not determine CPU count from /proc/interrupts\n");
        fclose(fp);
        return result;
    }
    
    printf("Detected %d CPUs from /proc/interrupts header\n", num_cpus);
    result.num_cpus = num_cpus;
    result.counts = calloc(num_cpus, sizeof(unsigned long long));
    if (!result.counts) {
        perror("Failed to allocate memory for TLB counts");
        result.num_cpus = 0;
        fclose(fp);
        return result;
    }

    rewind(fp);
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "TLB")) {
             printf("Found TLB line: %s", line);
            p = strchr(line, ':');
            if (!p) continue;
            p++; 

            for (int i = 0; i < num_cpus; i++) {
                while (*p == ' ' || *p == '\t') p++;
                if (*p >= '0' && *p <= '9') {
                    result.counts[i] = strtoull(p, &p, 10);
                } else {
                    break;
                }
            }
            break; 
        }
    }

    fclose(fp);
    return result;
}

void free_tlb_counts(tlb_counts_t *counts) {
    if (counts && counts->counts) {
        free(counts->counts);
        counts->counts = NULL;
        counts->num_cpus = 0;
    }
}

void print_tlb_diff(tlb_counts_t *before, tlb_counts_t *after) {
    int max_cpus = (before->num_cpus < after->num_cpus) ? before->num_cpus : after->num_cpus;
    unsigned long long total_before = 0;
    unsigned long long total_after = 0;

    printf("\nTLB Shootdowns by CPU:\n");
    printf("CPU #    Before      After       Diff\n");
    printf("---------------------------------------\n");

    for (int i = 0; i < max_cpus; i++) {
        unsigned long long diff = after->counts[i] - before->counts[i];
        printf("CPU%-2d: %10llu %10llu %10llu\n", i, before->counts[i], after->counts[i], diff);
        total_before += before->counts[i];
        total_after += after->counts[i];
    }

    printf("---------------------------------------\n");
    printf("Total: %10llu %10llu %10llu\n", total_before, total_after, total_after - total_before);
}

void print_usage(const char* program_name) {
    printf("Usage: %s [NUM_THREADS] [DURATION_SEC] [VICTIM_CPU]\n", program_name);
    printf("  NUM_THREADS  : Number of attacker threads (default: %d)\n", 22);
    printf("  DURATION_SEC : Test duration in seconds (default: %d)\n", 5);
    printf("  VICTIM_CPU   : Target CPU for TLB shootdown (default: %d)\n", 0);
}

void segv_handler(int sig) {
    // The victim thread will likely get a SEGV when an attacker unmaps the memory.
}

void *victim_reader(void *arg) {
    cpu_set_t m;
    CPU_ZERO(&m);
    CPU_SET(VICTIM_CPU, &m);
    if (sched_setaffinity(0, sizeof(m), &m) == -1) {
        perror("sched_setaffinity (victim)");
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = segv_handler;
    sigaction(SIGSEGV, &sa, NULL);

    while (running) {
        // Inline assembly trick to keep TLB region loaded.
        asm volatile("" ::"r"(*(volatile char *)shared_region) : "memory");
    }
    return NULL;
}

void *attacker_thread(void *arg) {
    int cpu = *(int *)arg;
    cpu_set_t m;
    CPU_ZERO(&m);
    CPU_SET(cpu, &m);
    if (sched_setaffinity(0, sizeof(m), &m) == -1) {
        perror("sched_setaffinity (attacker)");
    }

    while (running) {
        if (munmap(shared_region, PAGE_SIZE) == -1) {
            perror("munmap in thread");
        }

        void* new_region = mmap(shared_region, PAGE_SIZE, PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        
        if (new_region == MAP_FAILED) {
            perror("mmap in thread");
            break;
        }

        map_unmap_cycles++;
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc >= 2) {
        NUM_THREADS = atoi(argv[1]);
        if (NUM_THREADS < 1) {
            fprintf(stderr, "Error: NUM_THREADS must be at least 1\n");
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    
    if (argc >= 3) {
        DURATION_SEC = atoi(argv[2]);
        if (DURATION_SEC < 1) {
            fprintf(stderr, "Error: DURATION_SEC must be at least 1 second\n");
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    int num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    printf("System has %d CPUs available\n", num_cpus);
    
    if (argc >= 4) {
        VICTIM_CPU = atoi(argv[3]);
        if (VICTIM_CPU < 0 || VICTIM_CPU >= num_cpus) {
            fprintf(stderr, "Error: VICTIM_CPU is out of bounds (must be 0-%d)\n", num_cpus-1);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (NUM_THREADS < 1 || DURATION_SEC < 1) {
        fprintf(stderr, "Error: Invalid arguments.\n");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (VICTIM_CPU < 0 || VICTIM_CPU >= num_cpus) {
        fprintf(stderr, "Error: VICTIM_CPU is out of bounds (must be 0-%d)\n", num_cpus-1);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    shared_region = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared_region == MAP_FAILED) {
        perror("mmap");
        return EXIT_FAILURE;
    }
    memset(shared_region, 0, PAGE_SIZE);

    printf("Starting test with Victim CPU: %d, Attacker Threads: %d, Duration: %d seconds\n",
           VICTIM_CPU, NUM_THREADS, DURATION_SEC);

    tlb_counts_t initial_counts = get_tlb_counts();
    if (initial_counts.num_cpus == 0) {
        fprintf(stderr, "Failed to read initial TLB shootdown counts. Exiting.\n");
        munmap(shared_region, PAGE_SIZE);
        return EXIT_FAILURE;
    }

    pthread_t vtid;
    pthread_create(&vtid, NULL, victim_reader, NULL);

    pthread_t *tids = malloc(NUM_THREADS * sizeof(pthread_t));
    int *core_list = malloc(NUM_THREADS * sizeof(int));
    int c = 0, idx = 0;
    while (idx < NUM_THREADS) {
        if (c != VICTIM_CPU) {
            core_list[idx++] = c;
        }
        c = (c + 1) % num_cpus;
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&tids[i], NULL, attacker_thread, &core_list[i]);
    }

    sleep(DURATION_SEC);
    running = 0;

    pthread_join(vtid, NULL);
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(tids[i], NULL);
    }
    
    tlb_counts_t final_counts = get_tlb_counts();

    munmap(shared_region, PAGE_SIZE);
    free(tids);
    free(core_list);
    
    printf("\nTest finished.\n");
    printf("Total mmap/munmap cycles: %llu → %.1f k/s\n",
           map_unmap_cycles, (double)map_unmap_cycles / DURATION_SEC / 1000.0);
    
    if (final_counts.num_cpus > 0) {
        print_tlb_diff(&initial_counts, &final_counts);
    } else {
        fprintf(stderr, "Failed to read final TLB counts.\n");
    }
    
    free_tlb_counts(&initial_counts);
    free_tlb_counts(&final_counts);

    return EXIT_SUCCESS;
}