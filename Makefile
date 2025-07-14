# Makefile for IPI_munmap.c and IPI_membarrier.c and IPI_memprotect.c and check_race_window.c

# Compiler
CC = gcc

# Compiler flags
CFLAGS = -Wall -O2 -lm

# Linker flags
LDFLAGS = -lm

obj-m += IPI_Virtual.o

# Executable names
TARGETS = IPI_sched_affinity IPI_munmap IPI_membarrier IPI_memprotect IPI_futex check_race_window 

all: $(TARGETS)
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

IPI_munmap: IPI_munmap.c
	$(CC) $(CFLAGS) -o IPI_munmap IPI_munmap.c 

IPI_memprotect: IPI_memprotect.c
	$(CC) $(CFLAGS) -o IPI_memprotect IPI_memprotect.c

IPI_futex: IPI_futex.c
	$(CC) $(CFLAGS) -o IPI_futex IPI_futex.c
	
IPI_sched_affinity: IPI_sched_affinity.c
	$(CC) $(CFLAGS) -o IPI_sched_affinity IPI_sched_affinity.c
	
IPI_membarrier: IPI_membarrier.c
	$(CC) $(CFLAGS) -o IPI_membarrier IPI_membarrier.c

check_race_window: check_race_window.c
	$(CC) $(CFLAGS) -o check_race_window check_race_window.c $(LDFLAGS)

load:
	sudo insmod IPI_Virtual.ko

unload:
	sudo rmmod IPI_Virtual
	
dmesg:
	dmesg | tail -n 20

clean:
	rm -f $(TARGETS)
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean