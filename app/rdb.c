// RDB File implementation
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>

#include "hashtable.h"
#include "helper.h"
#include "rdb.h"


rdb_buffer_context* init_rdb_context(const char* file_name, size_t size) {
    rdb_buffer_context *buff = malloc(sizeof(rdb_buffer_context));
    if (buff == NULL) {
        error("Failed to allocate memory for buffer.");
        return NULL;
    }

    int fd = open(file_name, O_RDONLY);
    if (fd == -1) {
        error("Failed to open the provided RDB file.");
        free(buff);
        return NULL;
    }

    buff->fd = fd;
    buff->buffer = malloc(size);
    if (buff->buffer == NULL) {
        error("Failed to allocate memory for buffer data.");
        free(buff);
        return NULL;
    }

    buff->size = read(fd, buff->buffer, size);
    buff->pos = 0;
    buff->in_segment = 0;

    return buff;
}

unsigned char read_byte_from_buffer(rdb_buffer_context* context) {
    if (context->pos >= context->size) {
        // Try to read file if we have exhausted all of the buffer
        context->size = read(context->fd, context->buffer, context->size);
        if (context->size == 0) {
            return 0; // Changed from NULL to 0 for EOF or read error
        }
        context->pos = 0;
    }

    return context->buffer[context->pos++];
}


int check_and_fill_buffer(rdb_buffer_context* context, size_t needed_bytes) {
    if (needed_bytes == 0) {
        return 1;
    }

    size_t bytes_available = context->size - context->pos;

    if (bytes_available >= needed_bytes) {
        return 1;
    }

    // Copy the remaining items to a temp memory
    // Read from the buffer and fill it.
    // This will be bad if we read in huge chunks, need to find a better way.
    memmove(context->buffer, context->buffer + context->pos, bytes_available);
    context->pos = 0;
    context->size = bytes_available;

    // Refill the buffer with new data
    size_t bytes_read = read(context->fd, context->buffer + context->size, RDB_READ_BUFFER_SIZE - bytes_available);
    if (bytes_read <= 0) {
        return 0;
    }

    context->size += bytes_read;

    return (context->size - context->pos) > 0;

}



// -------------------------- Segment Level Parsers------------------------

void parse_metadata_section(rdb_buffer_context* context) {
    // Read 2 string encoders for now
    unsigned char* key = parse_string_encoding(context, NULL);
    if (key == NULL) {
        error("Failed to parse key from metadata section.");
        return;
    }
    unsigned char* value = parse_string_encoding(context, NULL);
    if (value == NULL) {
        error("Failed to parse value from metadata section.");
        free(key);
        return;
    }


    // TEMP: Print the key-value pair
    printf("Key: %s, Value: %s\n", key, value);
    free(value);
}


//--------------------------------------------------------------------------


// -------------------------- RDB Encoding Parsers -------------------------

uint64_t parse_size_encoding(rdb_buffer_context* context) {
    unsigned char first_byte = read_byte_from_buffer(context);
    if (first_byte == 0) return 0; // Changed from NULL to 0

    uint8_t type = (first_byte & 0xC0);
    
    if (type == 0x00) {
        // If the first two bits are 0b00:
        // The size is the remaining 6 bits of the byte.
        // In this example, the size is 10: */
        // 0A
        // 00001010
       return first_byte & 0x3F;
    } else if (type == 0x40){
        //  If the first two bits are 0b01:
        //  The size is the next 14 bits
        //  (remaining 6 bits in the first byte, combined with the next byte),
        //  in big-endian (read left-to-right).
        //  In this example, the size is 700: */
        //  42 BC
        //  01000010 10111100
        unsigned char second_byte = read_byte_from_buffer(context);
        if (second_byte == 0) return 0; // Changed from NULL to 0

        return ((first_byte & 0x3F) << 8) | second_byte;
    } else if (type == 0x80){
        // If the first two bits are 0b10:
        // Ignore the remaining 6 bits of the first byte.
        // The size is the next 4 bytes, in big-endian (read left-to-right).
        // In this example, the size is 17000: */
        // 80 00 00 42 68
        // 10000000 00000000 00000000 01000010 01101000       
       
        check_and_fill_buffer(context, 4);

        return (context->buffer[context->pos++] << 24) |
                (context->buffer[context->pos++] << 16) |
                (context->buffer[context->pos++] << 8) |
                 context->buffer[context->pos++];
    } else if (type == 0xC0){
        // This is just sized encoded string
        return 0;
    }

    return 0;
}

unsigned char* parse_string_encoding(rdb_buffer_context* context, size_t* size) {
    unsigned char first_byte = read_byte_from_buffer(context);
    if (first_byte == 0) return NULL;

    uint8_t bytes_to_read = 0;

    if (first_byte >= 0xC0 && first_byte <= 0xC2) {
        // /* The 0xC0 size indicates the string is an 8-bit integer.
        // In this example, the string is "123". */
        // C0 7B

        // /* The 0xC1 size indicates the string is a 16-bit integer.
        // The remaining bytes are in little-endian (read right-to-left).
        // In this example, the string is "12345". */
        // C1 39 30

        // /* The 0xC2 size indicates the string is a 32-bit integer.
        // The remaining bytes are in little-endian (read right-to-left),
        // In this example, the string is "1234567". */
        // C2 87 D6 12 00
        bytes_to_read = 1 << (first_byte & 0x03);  // maps C0 -> 1, C1 -> 2, C2 -> 4
    } else if (first_byte == 0xC3) {
        return NULL;  // LZF not supported
    } else {
        bytes_to_read = first_byte;
    }

    if (bytes_to_read == 0) return NULL;

    check_and_fill_buffer(context, bytes_to_read);
    unsigned char* result = malloc(bytes_to_read + 1);
    if (result == NULL) {
        error("Failed to allocate memory for string.");
        return NULL;
    }
    memcpy(result, context->buffer + context->pos, bytes_to_read);
    context->pos += bytes_to_read;
    result[bytes_to_read] = '\0';

    if (size != NULL) {
        *size = bytes_to_read;  // Store the actual string length
    }

    return result;
}


// -------------------------------------------------------------------------

// Main API for Loading from RDB file
ht_table* load_from_rdb_file(const char* file_path) {
    // Read the rest of file into a buffer
    rdb_buffer_context *context = init_rdb_context(file_path, RDB_READ_BUFFER_SIZE);
    if (context == NULL) {
        error("Failed to initialize buffer.");
        goto cleanup_context;
    }

    ht_table *ht = ht_create();
    if (ht == NULL) {
        error("Failed to create hash table.");
        goto cleanup_context;
    }

    // Begin the parsing
    // Header Segment (Just gonna ignore it for now)
    // ----------------------------#
    // 52 45 44 49 53              # Magic String "REDIS"
    // 30 30 30 33                 # RDB Version Number as ASCII string. "0003" = 3
    // ----------------------------
    // Skip the first 9 bytes
    if (lseek(context->fd, 9, SEEK_SET) == -1) {
        error("Failed to seek in the file.");
        goto cleanup_context;
    }


    while(1) {
        unsigned char byte = read_byte_from_buffer(context);
        if (byte == 0) { // End of file or read error
            break;
        }

        switch (byte) {
            case 0xFA:
                // FA                             // Indicates the start of a metadata subsection.
                // 09 72 65 64 69 73 2D 76 65 72  // The name of the metadata attribute (string encoded): "redis-ver".
                // 06 36 2E 30 2E 31 36           // The value of the metadata attribute (string encoded): "6.0.16".
                context->in_segment=1; // We are in Metadata Section
                parse_metadata_section(context);
                exit(1);
                break;
        
            default:
                break;
            }
    }

cleanup_file:
    close(context->fd);

cleanup_context:
    if (ht == NULL) {
        ht_destroy(ht);
        return NULL;
    }
    return ht;
}