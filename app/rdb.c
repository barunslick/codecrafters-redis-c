// RDB File implementation
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

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
    if (fd < 0) {
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
    buff->eof_segment = 0;
    return buff;
}

int read_byte_from_buffer(rdb_buffer_context* context) {
    if (context->pos >= context->size) {
        // Try to read file if we have exhausted all of the buffer
        context->size = read(context->fd, context->buffer, context->size);
        if (context->size == 0) {
            return -1; // Return -1 for EOF or read error
        }
        context->pos = 0;
    }

    return (int)(context->buffer[context->pos++]);
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
    // Read the metadata section
    // FA                             // Indicates the start of a metadata subsection.
    // 09 72 65 64 69 73 2D 76 65 72  // The name of the metadata attribute (string encoded): "redis-ver".
    // 06 36 2E 30 2E 31 36           // The value of the metadata attribute (string encoded): "6.0.16".

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

    // TEMP: Only Print the key-value pair for now
    printf("Key: %s, Value: %s\n", key, value);
    free(key);
    free(value);

}


void parse_database_section(ht_table* ht, rdb_buffer_context* context) {
    // FE  // Indicates the start of a database subsection.
    // 00  /* The index of the database (size encoded).
    //     Here, the index is 0. */
    // 
    // FB // Indicates that hash table size information follows.
    // 03 /* The size of the hash table that stores the keys and values (size encoded).
    //     Here, the total key-value hash table size is 3. */
    // 02/* The size of the hash table that stores the expires of the keys (size encoded).
    //     Here, the number of keys with an expiry is 2. */
    uint64_t db_index = parse_size_encoding(context);
    if (db_index < 0) {
        error("Failed to parse database index.");
        return;
    }

    int hash_table_type = read_byte_from_buffer(context);
    if (hash_table_type < 0) {
        error("Failed to read hash table type.");
        return;
    }
    unsigned char hash_table_byte = (unsigned char)hash_table_type;
    if (hash_table_byte != 0xFB) {
        error("Invalid hash table type.");
        return;
    }

    uint64_t hash_table_size = parse_size_encoding(context);
    if (hash_table_size < 0) {
        error("Failed to parse hash table size.");
        return;
    }
    uint64_t expires_size = parse_size_encoding(context);
    if (expires_size < 0) {
        error("Failed to parse expires size.");
        return;
    }
    printf("DB Index: %d, Hash Table Size: %lu, Expires Size: %lu\n", db_index, hash_table_size, expires_size);


    unsigned char * key;
    unsigned char * value;
    int key_type;
    unsigned char value_type;
    uint64_t expire_time;
    
    while (1 && !context->eof_segment) {
        key_type = read_byte_from_buffer(context);
        if (key_type < 0) {
            break; // End of file or read error
        }
        unsigned char key_byte = (unsigned char)key_type;
        switch (key_byte) {
            case 0x00:
                // String type
                // 00                       /* The 1-byte flag that specifies the value’s type and encoding.
                                            // Here, the flag is 0, which means "string." */
                // 06 66 6F 6F 62 61 72     // The name of the key (string encoded). Here, it's "foobar".
                // 06 62 61 7A 71 75 78     // The value (string encoded). Here, it's "bazqux".
                key = parse_string_encoding(context, NULL);
                if (key == NULL) {
                    error("Failed to parse key from metadata section.");
                    return;
                }
                value = parse_string_encoding(context, NULL);
                if (value == NULL) {
                    error("Failed to parse value from metadata section.");
                    free(key);
                    free(value);
                    return;
                }
                printf("Key: %s, Value: %s\n", key, value);
                if(ht_set(ht, key, value, 0) == NULL) {
                    error("Failed to set key-value pair in hash table.");
                    free(key);
                    free(value);
                    return;
                }
                continue;
            case 0xFC:
                // With expire in milliseconds
                // FC                       /* Indicates that this key ("foo") has an expire,
                                            // and that the expire timestamp is expressed in milliseconds. */
                // 15 72 E7 07 8F 01 00 00  /* The expire timestamp, expressed in Unix time,
                                            // stored as an 8-byte unsigned long, in little-endian (read right-to-left).
                                            // Here, the expire timestamp is 1713824559637. */
                // 00                       // Value type is string.
                // 03 66 6F 6F              // Key name is "foo".
                // 03 62 61 72              // Value is "bar".

                check_and_fill_buffer(context, 8);
                expire_time = (uint64_t)context->buffer[context->pos++] |
                                ((uint64_t)context->buffer[context->pos++] << 8)  |
                                ((uint64_t)context->buffer[context->pos++] << 16) |
                                ((uint64_t)context->buffer[context->pos++] << 24) |
                                ((uint64_t)context->buffer[context->pos++] << 32) |
                                ((uint64_t)context->buffer[context->pos++] << 40) |
                                ((uint64_t)context->buffer[context->pos++] << 48) |
                                ((uint64_t)context->buffer[context->pos++] << 56);

                value_type = read_byte_from_buffer(context);
                if (value_type != 0x00) {
                    error("Value type not supported for now.");
                    free(key);
                    free(value);
                    return;
                }

                key = parse_string_encoding(context, NULL);
                if (key == NULL) {
                    error("Failed to parse key from metadata section.");
                    return;
                }
                value = parse_string_encoding(context, NULL);
                if (value == NULL) {
                    error("Failed to parse value from metadata section.");
                    free(key);
                    free(value);
                    return;
                }
                printf("Key: %s, Value: %s, Expire Time: %lu\n", key, value, expire_time);
                ht_set(ht, key, value, expire_time);
                continue;
            case 0xFD:
                // With expire in seconds
                //FD                       /* Indicates that this key ("baz") has an expire,
                                            //and that the expire timestamp is expressed in seconds. */
                //52 ED 2A 66              /* The expire timestamp, expressed in Unix time,
                                            //stored as an 4-byte unsigned integer, in little-endian (read right-to-left).
                                            //Here, the expire timestamp is 1714089298. */
                //00                       // Value type is string.
                //03 62 61 7A              // Key name is "baz".
                //03 71 75 78              // Value is "qux".
                check_and_fill_buffer(context, 4);
                expire_time = (uint64_t)context->buffer[context->pos++] |
                                    ((uint64_t)context->buffer[context->pos++] << 8)  |
                                    ((uint64_t)context->buffer[context->pos++] << 16) |
                                    ((uint64_t)context->buffer[context->pos++] << 24);


                value_type = read_byte_from_buffer(context);
                if (value_type != 0x00) {
                    error("Value type not supported for now.");
                    free(key);
                    free(value);
                    return;
                }

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
                ht_set(ht, key, value, expire_time);
                printf("Key: %s, Value: %s, Expire Time(in seconds): %lu\n", key, value, expire_time * 1000);
                continue;
            case 0xFF:
                printf("End of file segment.\n");
                context->eof_segment = 1;
                break;
            default:
                break;
        }

        free(key);
        free(value);
    }
}


//--------------------------------------------------------------------------


// -------------------------- RDB Encoding Parsers -------------------------

uint64_t parse_size_encoding(rdb_buffer_context* context) {
    int byte = read_byte_from_buffer(context);
    if (byte < 0) return 0; // Changed to check for negative value

    unsigned char first_byte = (unsigned char)byte;
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
        int second_byte_val = read_byte_from_buffer(context);
        if (second_byte_val < 0) return 0; // Changed to check for negative value
        unsigned char second_byte = (unsigned char)second_byte_val;

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
    int byte = read_byte_from_buffer(context);
    if (byte < 0) {
        error("Failed to read byte from buffer.");
        return NULL;  // Check for negative value (error/EOF)
    }

    unsigned char first_byte = (unsigned char)byte;
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
void load_from_rdb_file(ht_table* ht, const char* file_path) {
    // Read the rest of file into a buffer
    rdb_buffer_context *context = init_rdb_context(file_path, RDB_READ_BUFFER_SIZE);
    if (context == NULL && errno != ENOENT) {
        error("Failed to initialize buffer.");
        goto cleanup;
    } else if (context == NULL) {
        printf("File not found. Treating it as empty.\n");
        return;
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
        goto cleanup;
    }
    context->pos += 8;

    while(1) {
        if (context->eof_segment == 1) goto cleanup; // End of file. Leave it for now

        int byte = read_byte_from_buffer(context);
        if (byte < 0) { // End of file or read error
            break;
        }

        unsigned char first_byte = (unsigned char)byte;
        switch (first_byte) {
            case 0xFA:
                // Metadata section
                parse_metadata_section(context);
                continue;
            case 0xFE:
                // Database section
                parse_database_section(ht, context);
                continue;
            case 0xFF:
                // End of file
                goto cleanup;
            default:
                break;
        }
    }

cleanup:
    close(context->fd);
    free(context->buffer);

    return;
}