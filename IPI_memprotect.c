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
volatile unsigned long long protection_changes = 0;
void *protected_region = NULL;

// Calculate TLB
typedef struct
{
    int num_cpus;
    unsigned long long *counts;
} tlb_counts_t;

tlb_counts_t get_tlb_counts(void)
{
    tlb_counts_t result;
    result.num_cpus = 0;
    result.counts = NULL;

    FILE *fp = fopen("/proc/interrupts", "r");
    if (!fp)
    {
        perror("Failed to open /proc/interrupts");
        return result;
    }

    char line[4096];

    if (!fgets(line, sizeof(line), fp))
    {
        fclose(fp);
        return result;
    }

    int num_cpus = 0;
    char *p = line;
    while (*p)
    {

        while (*p && (*p == ' ' || *p == '\t'))
            p++;

        if (strncmp(p, "CPU", 3) == 0)
        {
            num_cpus++;

            while (*p && !(*p == ' ' || *p == '\t'))
                p++;
        }
        else
        {

            break;
        }
    }

    if (num_cpus == 0)
    {
        printf("Error: Could not determine CPU count from /proc/interrupts\n");
        fclose(fp);
        return result;
    }

    printf("Detected %d CPUs from /proc/interrupts header\n", num_cpus);

    result.num_cpus = num_cpus;
    result.counts = (unsigned long long *)calloc(num_cpus, sizeof(unsigned long long));
    if (!result.counts)
    {
        perror("Failed to allocate memory for TLB counts");
        result.num_cpus = 0;
        fclose(fp);
        return result;
    }

    rewind(fp);

    fgets(line, sizeof(line), fp);

    int found_tlb = 0;

    while (fgets(line, sizeof(line), fp))
    {

        for (char *lc = line; *lc; lc++)
        {
            if (*lc >= 'A' && *lc <= 'Z')
                *lc = *lc + ('a' - 'A');
        }

        if (strstr(line, "tlb") != NULL)
        {
            printf("Found TLB line: %s", line);
            found_tlb = 1;

            char *p = line;

            while (*p && *p != ':')
                p++;
            if (*p == ':')
                p++;

            for (int i = 0; i < num_cpus; i++)
            {

                while (*p && (*p == ' ' || *p == '\t'))
                    p++;

                if (*p && (*p >= '0' && *p <= '9'))
                {
                    result.counts[i] = strtoull(p, &p, 10);
                }
                else
                {

                    break;
                }
            }
        }
    }

    fclose(fp);

    return result;
}

void free_tlb_counts(tlb_counts_t *counts)
{
    if (counts && counts->counts)
    {
        free(counts->counts);
        counts->counts = NULL;
        counts->num_cpus = 0;
    }
}

void print_tlb_diff(tlb_counts_t *before, tlb_counts_t *after)
{
    int max_cpus = (before->num_cpus < after->num_cpus) ? before->num_cpus : after->num_cpus;

    printf("TLB Shootdowns by CPU:\n");
    printf("CPU #    Before      After       Diff\n");
    printf("-----------------------------------\n");

    unsigned long long total_before = 0;
    unsigned long long total_after = 0;

    for (int i = 0; i < max_cpus; i++)
    {
        unsigned long long diff = after->counts[i] - before->counts[i];
        printf("CPU%-2d: %10llu %10llu %10llu\n",
               i, before->counts[i], after->counts[i], diff);

        total_before += before->counts[i];
        total_after += after->counts[i];
    }

    printf("-----------------------------------\n");
    printf("Total: %10llu %10llu %10llu\n",
           total_before, total_after, total_after - total_before);
}

void print_usage(const char* program_name) {
    printf("Usage: %s [NUM_THREADS] [DURATION_SEC] [VICTIM_CPU]\n", program_name);
    printf("  NUM_THREADS  : Number of attacker threads (default: %d)\n", NUM_THREADS);
    printf("  DURATION_SEC : Test duration in seconds (default: %d)\n", DURATION_SEC);
    printf("  VICTIM_CPU   : Target CPU for TLB shootdown (default: %d)\n", VICTIM_CPU);
}

void *victim_reader(void *arg)
{
    cpu_set_t m;
    CPU_ZERO(&m);
    CPU_SET(VICTIM_CPU, &m);
    if (sched_setaffinity(0, sizeof(m), &m) == -1)
        perror("sched_setaffinity (victim)");
    while (running)
    {
        // fancy method of loading protected_region efficiently
        asm volatile("" ::"r"(*(volatile char *)protected_region) : "memory");
    }
    return NULL;
}

void *protection_changer(void *arg)
{
    int cpu = *(int *)arg;
    cpu_set_t m;
    CPU_ZERO(&m);
    CPU_SET(cpu, &m);
    if (sched_setaffinity(0, sizeof(m), &m) == -1)
        perror("sched_setaffinity (attacker)");

    struct sched_param p = {.sched_priority = 99};
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);

    while (running)
    {
        mprotect(protected_region, PAGE_SIZE, PROT_READ);
        mprotect(protected_region, PAGE_SIZE, PROT_READ | PROT_WRITE);
        protection_changes += 2;
        asm volatile("" ::: "memory");
    }
    return NULL;
}

int main(int argc, char *argv[])
{

// Process command line arguments
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

    protected_region = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (protected_region == MAP_FAILED)
    {
        perror("mmap");
        return EXIT_FAILURE;
    }
    memset(protected_region, 0, PAGE_SIZE);

    printf("Victim core: %d, attackers: %d threads, time: %d s\n",
           VICTIM_CPU, NUM_THREADS, DURATION_SEC);

    // Measure TLB entrances
    tlb_counts_t initial_counts = get_tlb_counts();
    printf("Read TLB counts for %d CPUs\n", initial_counts.num_cpus);

    if (initial_counts.num_cpus == 0 || initial_counts.counts == NULL)
    {
        fprintf(stderr, "Failed to read TLB shootdown counts\n");
        return EXIT_FAILURE;
    }

    pthread_t vtid;
    pthread_create(&vtid, NULL, victim_reader, NULL);

    int *core_list = malloc(NUM_THREADS * sizeof(int));
    int c = 0, idx = 0;
    while (idx < NUM_THREADS)
    {
        if (c != VICTIM_CPU)
            core_list[idx++] = c;
        c = (c + 1) % num_cpus;
    }

    pthread_t *t = malloc(NUM_THREADS * sizeof(pthread_t));
    for (int i = 0; i < NUM_THREADS; i++){
        pthread_create(&t[i], NULL, protection_changer, &core_list[i]);
    }

    sleep(DURATION_SEC);
    running = 0;

    pthread_join(vtid, NULL);
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(t[i], NULL);

    munmap(protected_region, PAGE_SIZE);
    free(core_list);
    free(t);

    printf("Protection changes: %llu → %.1f k/s\n",
           protection_changes,
           protection_changes / (double)DURATION_SEC / 1000.0);

    tlb_counts_t final_counts = get_tlb_counts();

    if (final_counts.num_cpus > 0 && final_counts.counts != NULL)
    {
        print_tlb_diff(&initial_counts, &final_counts);
    }
    else
    {
        printf("Failed to read final TLB counts\n");
    }

    return 0;
}