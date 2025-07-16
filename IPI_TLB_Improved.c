/*
you can have more than 23 threads when using this IPI storm this was done for testing 
purposes but has no positive result so can effictley be ignored (it will loop back to the start)
*/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sched.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#define PAGE_SIZE 4096
#define MAP_SIZE (PAGE_SIZE * 256)

int NUM_THREADS = 22; 
int DURATION_SEC = 5;  
int VICTIM_CPU = 0;

static volatile int running = 1;
static volatile unsigned long long total_ops = 0;


typedef struct {
    int cpu_id;
    unsigned long long operations;
    void *shared_memory;    // Memory shared with victim different per thread
} thread_stats_t;

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

void* victim_thread(void *arg) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(VICTIM_CPU, &mask);
    
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = segv_handler;
    sigaction(SIGSEGV, &sa, NULL);

    if (sched_setaffinity(0, sizeof(mask), &mask) == -1) {
        fprintf(stderr, "Victim thread: Failed to set CPU affinity to %d: %s\n", 
                VICTIM_CPU, strerror(errno));
        return NULL;
    }
    
    printf("Victim thread running on CPU %d\n", VICTIM_CPU);
    
    // Get shared memory pointers
    void **memory_regions = (void **)arg;
    volatile unsigned char temp[8] = {0};
    
    // Continuously access all regions to keep them in TLB unlike Normal tests
    while (running) {
        for (int i = 0; i < NUM_THREADS; i++) {
            if (memory_regions[i] != NULL && memory_regions[i] != MAP_FAILED) {
                volatile unsigned char *region = memory_regions[i];
                
                // Touch every page in the region
                for (size_t offset = 0; offset < MAP_SIZE; offset += PAGE_SIZE) {
                    temp[0] ^= region[offset];
                }
            }
        }
        
    }
    
    if (temp[0] == 0xFF) {
        printf("Should never print: %d\n", temp[0]);
    }
    
    return NULL;
}

void* hammer_thread(void *arg) {
    thread_stats_t *stats = (thread_stats_t *)arg;
    int cpu_id = stats->cpu_id;
    
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu_id, &mask);
    
    if (sched_setaffinity(0, sizeof(mask), &mask) == -1) {
        fprintf(stderr, "Hammer thread: Failed to set CPU affinity to %d: %s\n", 
                cpu_id, strerror(errno));
        return NULL;
    }
    
    printf("Hammer thread running on CPU %d → targeting CPU %d\n", cpu_id, VICTIM_CPU);
    
    stats->operations = 0;
    
    stats->shared_memory = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE,
                               MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    
    if (stats->shared_memory == MAP_FAILED) {
        perror("mmap in hammer thread");
        return NULL;
    }
    
    memset(stats->shared_memory, (unsigned char)cpu_id, MAP_SIZE);
    
    usleep(100000);
    
    while (running) {
        // Mprotect for tlb shootdowns
        if (mprotect(stats->shared_memory, MAP_SIZE, PROT_READ) == 0) {
            stats->operations++;
        }
        
        if (mprotect(stats->shared_memory, MAP_SIZE, PROT_READ | PROT_WRITE) == 0) {
            stats->operations++;
        }
        
        // Touch all Pages
        if ((stats->operations % 100) == 0) {
            volatile unsigned char *p = stats->shared_memory;
            for (size_t i = 0; i < MAP_SIZE; i += PAGE_SIZE) {
                p[i] = (unsigned char)(i & 0xFF);
            }
        }
    }
    
    if (stats->shared_memory != MAP_FAILED && stats->shared_memory != NULL) {
        munmap(stats->shared_memory, MAP_SIZE);
        stats->shared_memory = NULL;
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
    
    cpu_set_t available;
    CPU_ZERO(&available);
    if (sched_getaffinity(0, sizeof(available), &available) == -1) {
        perror("sched_getaffinity");
        return EXIT_FAILURE;
    }
    
    if (!CPU_ISSET(VICTIM_CPU, &available)) {
        fprintf(stderr, "CPU %d is not available!\n", VICTIM_CPU);
        return EXIT_FAILURE;
    }
    
    if (num_cpus < 2) {
        fprintf(stderr, "This test requires at least 2 CPUs\n");
        return EXIT_FAILURE;
    }
    
    tlb_counts_t initial_counts = get_tlb_counts();
    if (initial_counts.num_cpus == 0 || initial_counts.counts == NULL) {
        fprintf(stderr, "Failed to read TLB shootdown counts\n");
        return EXIT_FAILURE;
    }
    
    printf("Read TLB counts for %d CPUs\n", initial_counts.num_cpus);
    
    thread_stats_t *thread_stats = (thread_stats_t *)malloc(NUM_THREADS * sizeof(thread_stats_t));
    pthread_t *hammer_threads = (pthread_t *)malloc(NUM_THREADS * sizeof(pthread_t));
    pthread_t victim_thread_id;
    void **shared_memory_ptrs = (void **)calloc(NUM_THREADS, sizeof(void *));
    
    if (!thread_stats || !hammer_threads || !shared_memory_ptrs) {
        perror("Memory allocation failed");
        free(thread_stats);
        free(hammer_threads);
        free(shared_memory_ptrs);
        free_tlb_counts(&initial_counts);
        return EXIT_FAILURE;
    }
    
    int thread_count = 0;
    int next_cpu = 0;

    printf("\nStarting TLB Shootdown Test:\n");
    printf(" - Target CPU: %d\n", VICTIM_CPU);
    printf(" - Duration: %d seconds\n", DURATION_SEC);
    printf(" - Attacker CPUs: ");

    while (thread_count < NUM_THREADS) {
        if (next_cpu != VICTIM_CPU) {
            thread_stats[thread_count].cpu_id = next_cpu;
            printf("%d ", next_cpu);
            thread_count++;
        }
        next_cpu = (next_cpu + 1) % num_cpus;
        if (next_cpu == 0 && thread_count == 0) {
            break;
        }
    }
    printf("\n\n");
    
    for (int i = 0; i < thread_count; i++) {
        if (pthread_create(&hammer_threads[i], NULL, hammer_thread, &thread_stats[i])) {
            perror("pthread_create for hammer thread");
            running = 0;
            goto cleanup_early;
        }
    }
    
    sleep(1);

    for (int i = 0; i < thread_count; i++) {
        shared_memory_ptrs[i] = thread_stats[i].shared_memory;
    }
    
    if (pthread_create(&victim_thread_id, NULL, victim_thread, shared_memory_ptrs)) {
        perror("pthread_create for victim thread");
        running = 0;
        goto cleanup_early;
    }
    
    time_t start_time = time(NULL);
    time_t last_update = start_time;
    unsigned long long last_ops = 0;
    
    while (running && (time(NULL) - start_time < DURATION_SEC)) {
        sleep(1);
        
        unsigned long long current_ops = 0;
        for (int i = 0; i < thread_count; i++) {
            current_ops += thread_stats[i].operations;
        }
        
        time_t now = time(NULL);
        float ops_per_sec = (float)(current_ops - last_ops) / (float)(now - last_update);
        
        printf("\rOperations: %llu (%.2f K/sec)", current_ops, ops_per_sec / 1000.0f);
        fflush(stdout);
        
        last_update = now;
        last_ops = current_ops;
    }
    
    running = 0;
    printf("\n\nTest complete, cleaning up...\n");
    
cleanup_early:
    pthread_join(victim_thread_id, NULL);
    
    unsigned long long final_ops = 0;
    for (int i = 0; i < thread_count; i++) {
        pthread_join(hammer_threads[i], NULL);
        final_ops += thread_stats[i].operations;
    }
    
    tlb_counts_t final_counts = get_tlb_counts();
    
    if (final_counts.num_cpus > 0 && final_counts.counts != NULL) {
        print_tlb_diff(&initial_counts, &final_counts);
    } else {
        printf("Failed to read final TLB counts\n");
    }
    
    free_tlb_counts(&initial_counts);
    free_tlb_counts(&final_counts);
    free(thread_stats);
    free(hammer_threads);
    free(shared_memory_ptrs);
    
    time_t elapsed = time(NULL) - start_time;
    printf("\nTest ran for %ld seconds\n", elapsed);
    printf("Total operations performed: %llu\n", final_ops);
    printf("Average rate: %.2f K/sec\n", (float)final_ops / (float)elapsed / 1000.0f);
    
    return EXIT_SUCCESS;
}
