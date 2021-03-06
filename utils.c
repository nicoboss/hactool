#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include "utils.h"
#include "filepath.h"
#include "sha.h"

uint32_t align(uint32_t offset, uint32_t alignment) {
    uint32_t mask = ~(alignment-1);

    return (offset + (alignment-1)) & mask;
}

uint64_t align64(uint64_t offset, uint64_t alignment) {
    uint64_t mask = ~(uint64_t)(alignment-1);

    return (offset + (alignment-1)) & mask;
}

/* Print a magic number. */
void print_magic(const char *prefix, uint32_t magic) {
    printf("%s%c%c%c%c\n", prefix, (char)((magic >> 0) & 0xFF), (char)((magic >> 8) & 0xFF), (char)((magic >> 16) & 0xFF), (char)((magic >> 24) & 0xFF));
}


/* Taken mostly from ctrtool. */
void memdump(FILE *f, const char *prefix, const void *data, size_t size) {
    uint8_t *p = (uint8_t *)data;

    unsigned int prefix_len = strlen(prefix);
    size_t offset = 0;
    int first = 1;

    while (size) {
        unsigned int max = 32;

        if (max > size) {
            max = size;
        }

        if (first) {
            fprintf(f, "%s", prefix);
            first = 0;
        } else {
            fprintf(f, "%*s", prefix_len, "");
        }

        for (unsigned int i = 0; i < max; i++) {
            fprintf(f, "%02X", p[offset++]);
        }

        fprintf(f, "\n");

        size -= max;
    }
}

void save_file_section(FILE *f_in, uint64_t ofs, uint64_t total_size, filepath_t *filepath) {
    FILE *f_out = os_fopen(filepath->os_path, OS_MODE_WRITE);

    if (f_out == NULL) {
        fprintf(stderr, "Failed to open %s!\n", filepath->char_path);
        return;
    }

    uint64_t read_size = 0x400000; /* 4 MB buffer. */
    unsigned char *buf = malloc(read_size);
    if (buf == NULL) {
        fprintf(stderr, "Failed to allocate file-save buffer!\n");
        exit(EXIT_FAILURE);
    }
    memset(buf, 0xCC, read_size); /* Debug in case I fuck this up somehow... */
    uint64_t end_ofs = ofs + total_size;
    fseeko64(f_in, ofs, SEEK_SET);
    while (ofs < end_ofs) {       
        if (ofs + read_size >= end_ofs) read_size = end_ofs - ofs;
        if (fread(buf, 1, read_size, f_in) != read_size) {
            fprintf(stderr, "Failed to read file!\n");
            exit(EXIT_FAILURE);
        }
        fwrite(buf, 1, read_size, f_out);
        ofs += read_size;
    }

    fclose(f_out);

    free(buf);
}


validity_t check_memory_hash_table(FILE *f_in, unsigned char *hash_table, uint64_t data_ofs, uint64_t data_len, uint64_t block_size, int full_block) {
    if (block_size == 0) {
        /* Block size of 0 is always invalid. */
        return VALIDITY_INVALID;
    }
    unsigned char cur_hash[0x20];
    uint64_t read_size = block_size;
    unsigned char *block = malloc(block_size);
    if (block == NULL) {
        fprintf(stderr, "Failed to allocate hash block!\n");
        exit(EXIT_FAILURE);
    }

    validity_t result = VALIDITY_VALID;
    unsigned char *cur_hash_table_entry = hash_table;
    for (uint64_t ofs = 0; ofs < data_len; ofs += read_size) {
        fseeko64(f_in, ofs + data_ofs, SEEK_SET);
        if (ofs + read_size > data_len) {
            /* Last block... */
            memset(block, 0, read_size);
            read_size = data_len - ofs;
        }
        
        if (fread(block, 1, read_size, f_in) != read_size) {
            fprintf(stderr, "Failed to read file!\n");
            exit(EXIT_FAILURE);
        }        
        sha256_hash_buffer(cur_hash, block, full_block ? block_size : read_size);
        if (memcmp(cur_hash, cur_hash_table_entry, 0x20) != 0) {
            result = VALIDITY_INVALID;
            break;
        }
        cur_hash_table_entry += 0x20;
    }
    free(block);

    return result;

}

validity_t check_file_hash_table(FILE *f_in, uint64_t hash_ofs, uint64_t data_ofs, uint64_t data_len, uint64_t block_size, int full_block) {
    if (block_size == 0) {
        /* Block size of 0 is always invalid. */
        return VALIDITY_INVALID;
    }    
    uint64_t hash_table_size = data_len / block_size;
    if (data_len % block_size) hash_table_size++;
    hash_table_size *= 0x20;
    unsigned char *hash_table = malloc(hash_table_size);
    if (hash_table == NULL) {
        fprintf(stderr, "Failed to allocate hash table!\n");
        exit(EXIT_FAILURE);
    }

    fseeko64(f_in, hash_ofs, SEEK_SET);
    if (fread(hash_table, 1, hash_table_size, f_in) != hash_table_size) {
        fprintf(stderr, "Failed to read file!\n");
        exit(EXIT_FAILURE);
    }

    validity_t result = check_memory_hash_table(f_in, hash_table, data_ofs, data_len, block_size, full_block);

    free(hash_table);

    return result;
}