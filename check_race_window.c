#define _GNU_SOURCE 
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <x86intrin.h>

#define TARGET_CPU 0 
#define NUM_ITERATIONS 100
#define WARMUP_ITERATIONS 1000 // Warmup Cache

static inline uint64_t get_cycles() {
    _mm_mfence();          // Memory fence to stop compiler from changing execution order
    uint64_t t = __rdtsc();
    _mm_lfence();          
    return t;
}

int pin_to_cpu(int cpu_id) {
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(cpu_id, &cpu_set);
    
    if (sched_setaffinity(0, sizeof(cpu_set), &cpu_set) == -1) {
        fprintf(stderr, "Failed to set CPU affinity to CPU %d: %s\n", 
                cpu_id, strerror(errno));
        return -1;
    }
    
    return 0;
}

// quick sort
int compare_uint64(const void *a, const void *b) {
    uint64_t ua = *(const uint64_t*)a;
    uint64_t ub = *(const uint64_t*)b;
    
    if (ua < ub) return -1;
    if (ua > ub) return 1;
    return 0;
}

void calculate_stats(uint64_t *measurements, int count,
                   double *avg, uint64_t *min, uint64_t *max, uint64_t *median, 
                   uint64_t *p95, uint64_t *p99, double *stddev, double *avg_trimmed) {
    qsort(measurements, count, sizeof(uint64_t), compare_uint64);
    
    *min = measurements[0];
    *max = measurements[count-1];
    
    // Calculate Mean
    uint64_t sum = 0;
    for (int i = 0; i < count; i++) {
        sum += measurements[i];
    }
    *avg = (double)sum / count;
    
    // Calculate trimmed average (not very informative i made this while testing but decided to keep it)
    int trim_count = count / 10; 
    int trimmed_count = count - 2 * trim_count;
    uint64_t trimmed_sum = 0;
    
    for (int i = trim_count; i < count - trim_count; i++) {
        trimmed_sum += measurements[i];
    }
    *avg_trimmed = (double)trimmed_sum / trimmed_count;
    
    // Calculate standard deviation
    double variance = 0;
    for (int i = 0; i < count; i++) {
        variance += pow(measurements[i] - *avg, 2);
    }
    variance /= count;
    *stddev = sqrt(variance);
    
    // Calculate median
    *median = (count % 2 == 0) ? 
        (measurements[count/2 - 1] + measurements[count/2]) / 2 :
        measurements[count/2];
    
    // Calculate Percentiles
    *p95 = measurements[(int)(count * 0.95)];
    *p99 = measurements[(int)(count * 0.99)];
}

int main(int argc, char *argv[]) {
    unsigned int target_cpu = TARGET_CPU;
    unsigned int num_iterations = NUM_ITERATIONS;
    int json_output = 0;  // Default NO json
    char *output_file = NULL;
    
    // Process command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cpu") == 0 && i+1 < argc) {
            target_cpu = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--iterations") == 0 && i+1 < argc) {
            num_iterations = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--json") == 0) {
            json_output = 1;
        } else if (strcmp(argv[i], "--output") == 0 && i+1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --cpu N          Run on CPU N (default: %d)\n", TARGET_CPU);
            printf("  --iterations N   Perform N iterations (default: %d)\n", NUM_ITERATIONS);
            printf("  --json           Output in JSON format\n");
            printf("  --output FILE    Write output to FILE\n");
            printf("  --help           Display this help message\n");
            return EXIT_SUCCESS;
        }
    }
    
    int available_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (target_cpu >= available_cpus) {
        fprintf(stderr, "CPU %d not available. System has %d CPUs.\n", 
                target_cpu, available_cpus);
        return EXIT_FAILURE;
    }
    
    if(num_iterations < 10){
        fprintf(stderr, "Minimum of 10 iterations are needed.\n");
        return EXIT_FAILURE;
    }

    if (!json_output) {
        printf("Measuring getpid() execution cycles on CPU %d\n", target_cpu);
        printf("System has %d available CPUs\n", available_cpus);
    }
    
    if (pin_to_cpu(target_cpu) != 0) {
        return EXIT_FAILURE;
    }

    int current_cpu = sched_getcpu();
    if (current_cpu != target_cpu) {
        fprintf(stderr, "Failed to pin to CPU %d (currently on CPU %d)\n", 
                target_cpu, current_cpu);
        return EXIT_FAILURE;
    }
    
    if (!json_output) {
        printf("Successfully pinned to CPU %d\n", current_cpu);
    }
    
    // Allocate memory for storing measurements
    uint64_t *measurements = (uint64_t*)malloc(sizeof(uint64_t) * num_iterations);
    if (!measurements) {
        perror("Failed to allocate memory for measurements");
        return EXIT_FAILURE;
    }
    
    // Warmup phase
    if (!json_output) {
        printf("Performing %d warmup iterations...\n", WARMUP_ITERATIONS);
    }
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        getpid();
    }
    
    if (!json_output) {
        printf("Performing %d measurement iterations...\n", num_iterations);
    }
    
    // Main measurement loop
    for (int i = 0; i < num_iterations; i++) {
        sleep(0.00000001);
        uint64_t start = get_cycles();
        getpid();
        uint64_t end = get_cycles();
        
        measurements[i] = end - start;
        
        // Progress indicator not necessary but it helps make sure the program is running because it can slow down drastically
        if (!json_output && i % (num_iterations / 10) == 0) {
	    printf("\rProgress: %d%%", (i * 100) / num_iterations);
            fflush(stdout);
        }
    }
    
    if (!json_output) {
        printf("\rProgress: 100%%\n");
    }
    
    double avg, stddev, avg_trimmed;
    uint64_t min, max, median, p95, p99;
    calculate_stats(measurements, num_iterations, &avg, &min, &max, &median, 
                   &p95, &p99, &stddev, &avg_trimmed);
    
    // Calculate histogram buckets 
    #define NUM_BUCKETS 20
    uint64_t bucket_size = (max - min) / NUM_BUCKETS;
    if (bucket_size == 0) bucket_size = 1;  // Avoid division by zero
    
    int buckets[NUM_BUCKETS] = {0};
    
    for (int i = 0; i < num_iterations; i++) {
        int bucket = (measurements[i] - min) / bucket_size;
        if (bucket >= NUM_BUCKETS) bucket = NUM_BUCKETS - 1;
        buckets[bucket]++;
    }
    
    // Setup output file
    FILE *output = stdout;
    if (output_file) {
        output = fopen(output_file, "w");
        if (!output) {
            perror("Failed to open output file");
            free(measurements);
            return EXIT_FAILURE;
        }
    }
    
    if (json_output) {
        // JSON output format
        // Miscellanious helps alot with organization
        fprintf(output, "{\n");
        fprintf(output, "  \"system_info\": {\n");
        fprintf(output, "    \"available_cpus\": %d,\n", available_cpus);
        fprintf(output, "    \"target_cpu\": %d,\n", target_cpu);
        fprintf(output, "    \"iterations\": %d,\n", num_iterations);
        fprintf(output, "    \"warmup_iterations\": %d\n", WARMUP_ITERATIONS);
        fprintf(output, "  },\n");
        
        // Statistics
        fprintf(output, "  \"statistics\": {\n");
        fprintf(output, "    \"min\": %lu,\n", min);
        fprintf(output, "    \"avg\": %.2f,\n", avg);
        fprintf(output, "    \"avg_trimmed\": %.2f,\n", avg_trimmed);
        fprintf(output, "    \"median\": %lu,\n", median);
        fprintf(output, "    \"stddev\": %.2f,\n", stddev);
        fprintf(output, "    \"p95\": %lu,\n", p95);
        fprintf(output, "    \"p99\": %lu,\n", p99);
        fprintf(output, "    \"max\": %lu,\n", max);
        fprintf(output, "  },\n");
        
        // Raw measurements
        fprintf(output, "  \"raw_measurements\": [\n");
        for (int i = 0; i < num_iterations; i++) {
            fprintf(output, "    %lu%s\n", measurements[i], (i < num_iterations - 1) ? "," : "");
        }
        fprintf(output, "  ]\n");
        fprintf(output, "}\n");
    } else {
        // Non-json output
        fprintf(output, "\n=== Results for getpid() on CPU %d ===\n", target_cpu);
        fprintf(output, "Minimum cycles:    %lu\n", min);
        fprintf(output, "Average cycles:    %.2f\n", avg);
        fprintf(output, "Trimmed average:   %.2f (ignored top/bottom 10%%)\n", avg_trimmed);
        fprintf(output, "Median cycles:     %lu\n", median);
        fprintf(output, "Std deviation:     %.2f\n", stddev);
        fprintf(output, "95th percentile:   %lu\n", p95);
        fprintf(output, "99th percentile:   %lu\n", p99);
        fprintf(output, "Maximum cycles:    %lu\n", max);
        
        // Print distribution histogram
        fprintf(output, "\n=== Distribution Histogram ===\n");
        for (int i = 0; i < NUM_BUCKETS; i++) {
            uint64_t lower = min + (i * bucket_size);
            uint64_t upper = min + ((i + 1) * bucket_size);
            fprintf(output, "%5lu - %5lu cycles: ", lower, upper);
            
            int percentage = (buckets[i] * 100) / num_iterations;
            int bar_length = (percentage * 60) / 100;
            
            for (int j = 0; j < bar_length; j++) {
                fprintf(output, "#");
            }
            fprintf(output, " %d%%\n", percentage);
        }

    }
    
    if (output_file && output != stdout) {
        fclose(output);
    }
    
    free(measurements);
    
    return EXIT_SUCCESS;
}
