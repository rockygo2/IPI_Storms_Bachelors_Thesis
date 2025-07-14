# IPI Storms - Bachelor's Thesis

This repository contains the code and measurement tools for the Bachelor's Thesis project on Inter-Processor Interrupt (IPI) Storms.

## Measurement Tool

This tool is used to measure system performance and behavior under various conditions.

### Options

The measurement tool accepts the following command-line arguments:

| **Option** | **Argument** | **Description** | **Default** |
| :--- | :--- | :--- |:---|
| `--cpu` | `N` | Binds the measurement tool to run on a specific CPU core `N`. | `TARGET_CPU` |
| `--iterations` | `N` | Sets the number of iterations `N` for the measurement loop. | `NUM_ITERATIONS` |
| `--json` |  | Outputs the results in JSON format for easy parsing. | N/A |
| `--output` | `FILE` | Writes the output of the measurement to a specified `FILE`. | stdout |
| `--help` |  | Displays the help message with all available options. | N/A |

### Example Usage

```bash
./measurement --cpu 2 --iterations 1000000 --json --output results.json
```

## IPI Storms Usage

The different IPI storm attack implementations share a common set of command-line arguments to control their execution.

### Arguments

The general syntax for running an IPI storm is:

```bash
./<storm_name> <NUM_THREADS> <DURATION> <VICTIM_CPU>
```

* **`NUM_THREADS`**: The number of malicious threads to spawn for generating the IPI storm.
* **`DURATION`**: The total time in seconds for which the storm should run.
* **`VICTIM_CPU`**: The target CPU core that the IPI storm will be directed against.

### Example

To run a hypothetical `storm_a` with 8 threads for 60 seconds against CPU core 3, you would use the following command:

```bash
./storm_a 8 60 3
```

### Building the Project

To compile all the IPI storm executables and the kernel module, run the following command from the root of the project directory:

```bash
make all
```
This will create the following executables: `IPI_sched_affinity`, `IPI_munmap`, `IPI_membarrier`, `IPI_memprotect`, `IPI_futex`, `check_race_window`, and compile the `IPI_Virtual.ko` kernel module.

To remove all compiled executables and kernel module object files, run:

```bash
make clean
```

## License
Apache License Version 2.0
