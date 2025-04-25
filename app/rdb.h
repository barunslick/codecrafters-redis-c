#include <stdio.h>
#include <stdint.h>


#define RDB_READ_BUFFER_SIZE 1024

typedef struct {
    int fd;
    int in_segment;
    unsigned char* buffer;
    size_t size;
    size_t pos;
} rdb_buffer_context;

// Buffer management functions
rdb_buffer_context* init_rdb_context(const char* file_name, size_t size);
unsigned char read_byte_from_buffer(rdb_buffer_context* buff);
int check_and_fill_buffer(rdb_buffer_context* context, size_t needed_bytes);

// Segment level parsers
void parse_metadata_section(rdb_buffer_context* context);

// RDB encoding parsers
uint64_t parse_size_encoding(rdb_buffer_context* context);
unsigned char* parse_string_encoding(rdb_buffer_context* context, size_t* size);

// Main API
ht_table* load_from_rdb_file(const char* filename);