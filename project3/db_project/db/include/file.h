#ifndef DB_FILE_H_
#define DB_FILE_H_

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#define PAGE_SIZE (4 * 1024)							// 4KiB
#define INITIAL_FILESIZE (10 * 1024 * 1024)				// 10MiB
#define INITIAL_PAGENUM (INITIAL_FILESIZE / PAGE_SIZE) 	// 2560
#define NUM_BUCKETS 31

typedef uint64_t pagenum_t;

struct __attribute__((packed)) slot_t {
    int64_t key;
    uint16_t size;
    uint16_t offset;
};

struct entry_t {
    int64_t key;
    uint64_t child;
};

struct page_t {
    union {
        uint64_t next_frpg;
        uint64_t parent;
    };
    union {
        uint64_t num_pages;
        struct {
            uint32_t is_leaf;
            uint32_t num_keys;
        };
    };
    uint64_t root_num;
    char reserved[88];
    uint64_t free_space;
    union { 
        uint64_t sibling;
        uint64_t left_child;
    };
    union { 
        union {
            slot_t slots[64];
            char values[3968];
        };
        entry_t entries[248];
    };
};

struct table_t {
    char pathname[256];
    int fd;
};

extern table_t tables[];

int64_t file_open_table_file(const char * pathname);
pagenum_t file_alloc_page(int64_t table_id);
void file_free_page(int64_t table_id, pagenum_t page_num);
void file_read_page(int64_t table_id, pagenum_t page_num, page_t * dest);
void file_write_page(int64_t table_id, pagenum_t page_num, const page_t * src);
void file_close_table_file();

#endif
