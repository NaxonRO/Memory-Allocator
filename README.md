# Memory Allocator in C

A minimal `malloc` / `calloc` / `realloc` / `free` implementation for Linux, built directly on the `brk()`, `mmap()` and `munmap()` system calls and packaged as a shared library (`libosmem.so`).

The implementation passes all **40/40** tests of the course test suite. Each test compares the exact sequence of system calls made by the allocator against a reference trace.

## API

```c
void *os_malloc(size_t size);
void *os_calloc(size_t nmemb, size_t size);
void *os_realloc(void *ptr, size_t size);
void  os_free(void *ptr);
```

## Design

Every block is preceded by a metadata header:

```c
struct block_meta {
	size_t size;
	int status;               // STATUS_FREE, STATUS_ALLOC or STATUS_MAPPED
	struct block_meta *prev;
	struct block_meta *next;
};
```

### Two allocation paths
- **Small blocks** (header + payload below 128 KB) live on the heap, which grows with `sbrk()`. Heap blocks form a **circular doubly linked list**.
- **Large blocks** get their own anonymous `mmap()` region and are released with `munmap()`. They are not part of the heap list.
- `os_calloc` uses a lower threshold, the page size (4 KB), before switching to `mmap()`.

### Heap preallocation
The first heap allocation reserves a whole 128 KB chunk with a single `sbrk()` call. This reduces the number of system calls for subsequent small allocations.

### Reusing free memory
- **Best fit:** the free block with the smallest size that is still large enough is reused.
- **Splitting:** if the chosen block is larger than needed and the remainder can hold a header plus at least 8 bytes, the remainder becomes a new free block.
- **Coalescing:** adjacent free blocks are merged before each search, which reduces fragmentation.
- **Expanding the last block:** if no block fits and the last heap block is free, the heap is extended by just the missing amount instead of allocating a new block.

### `os_realloc`
- **Shrinking** splits the block in place.
- **Growing** tries, in order: extend the block if it is the last one on the heap, absorb the next block if it is free, otherwise allocate a new block, copy the data and free the old one.
- Blocks move between the heap and `mmap` when the new size crosses the threshold.

All sizes are aligned to 8 bytes.

## Build & test

```bash
cd src && make                 # builds libosmem.so

cd ../tests
make check                     # builds the test snippets and runs run_tests.py
```

The checker traces system calls with `ltrace`, so it needs `ltrace` and Python 3. The test programs are in `tests/snippets/` and the reference traces in `tests/ref/`.

## Repository layout

```
src/osmem.c        allocator implementation
utils/             public header, block_meta definition, heap-free printf
tests/snippets/    40 test programs (malloc / calloc / realloc scenarios)
tests/ref/         reference syscall traces
tests/run_tests.py checker
```

## Credits

Developed as an assignment for the *Operating Systems* course at the Faculty of Automatic Control and Computers, Politehnica University of Bucharest (2024). The course team provided the interface, `utils/` and the test suite. The allocator in `src/osmem.c` is my own work.
