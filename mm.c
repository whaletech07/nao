#include <stdint.h>

// Forward declaration for logging
extern void serial_printf(const char* format, ...);

// Define an 8-byte alignment rule for modern CPU architecture efficiency
#define ALIGN(size) (((size) + 7) & ~7)

// 4 Megabyte managed heap space inside the BSS data segment
static uint8_t kernel_heap_space[4 * 1024 * 1024];

struct block_header {
    uint32_t size;     // Size of the usable data block trailing this header
    uint8_t  is_free;  // 1 if available to be grabbed, 0 if active
    struct block_header* next;
};

static struct block_header* heap_start = (struct block_header*)kernel_heap_space;
static int mm_initialized = 0;

void init_mm(void) {
    // Setup our initial giant free memory block spanning the entire heap space
    heap_start->size = sizeof(kernel_heap_space) - sizeof(struct block_header);
    heap_start->is_free = 1;
    heap_start->next = 0;
    
    mm_initialized = 1;
    serial_printf("[MM DEBUG] Kernel Memory Manager Online. Managed Heap: %d bytes\n", heap_start->size);
}

void* kmalloc(uint32_t size) {
    if (!mm_initialized) init_mm();

    uint32_t needed_size = ALIGN(size);
    struct block_header* current = heap_start;

    // First-Fit search loop
    while (current) {
        if (current->is_free && current->size >= needed_size) {
            
            // Check if we can split this block to preserve leftover memory fragment spaces
            if (current->size >= (needed_size + sizeof(struct block_header) + 8)) {
                struct block_header* next_block = (struct block_header*)((uint8_t*)current + sizeof(struct block_header) + needed_size);
                
                next_block->size = current->size - needed_size - sizeof(struct block_header);
                next_block->is_free = 1;
                next_block->next = current->next;
                
                current->size = needed_size;
                current->next = next_block;
            }
            
            current->is_free = 0;
            // Return the memory pointer right AFTER the header tracking metadata block
            return (void*)((uint8_t*)current + sizeof(struct block_header));
        }
        current = current->next;
    }

    serial_printf("[MM FATAL] KERNEL OUT OF MEMORY! Failed allocation size: %d\n", size);
    while(1); // Panic
    return 0;
}

void kfree(void* ptr) {
    if (!ptr) return;

    // Wind backward to look at the hidden tracking header prefix
    struct block_header* header = (struct block_header*)((uint8_t*)ptr - sizeof(struct block_header));
    header->is_free = 1;

    // Coalesce loop: Merge consecutive free blocks together to prevent fragmentation
    struct block_header* current = heap_start;
    while (current) {
        if (current->next && current->is_free && current->next->is_free) {
            current->size += sizeof(struct block_header) + current->next->size;
            current->next = current->next->next;
            // Don't advance 'current', check if the newly expanded block can merge again
        } else {
            current = current->next;
        }
    }
}