#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "resp.h"

void free_resp_data(RESPData *data) {
    if (data == NULL) return;
    
    switch (data->type) {
        case RESP_SIMPLE_STRING:
        case RESP_ERROR:
        case RESP_BULK_STRING:
            free(data->data.str);
            break;
        case RESP_ARRAY:
            for (size_t i = 0; i < data->data.array.count; i++) {
                free_resp_data(data->data.array.elements[i]);
                free(data->data.array.elements[i]);
            }
            free(data->data.array.elements);
            break;
        default:
            break;
    }
}

RESPData* parse_bulk_string(char **buf) {
    // Example: $3\r\nfoo\r\n
    // "foo"
    char *start = *buf;
    char *end = strchr(start, '\r');
    RESPData *data = malloc(sizeof(RESPData));
    data->type = RESP_BULK_STRING;

    long length = strtol(start + 1, NULL, 10);

    if (length < 0) {
        data->type = RESP_NULL;
        data->data.str = NULL;
        *buf = end + 2;
        return data;
    }

    *buf = end + 2; // Skip the "$3\r\n" part
    data->data.str = strndup(*buf, length); // Copy happens here.
    if (data->data.str == NULL) {
        printf("Error copying bulk string\n");
        free(data);
        return NULL;
    }

    *buf += length + 2; // Skip the "foo\r\n" part
    
    return data;
}

RESPData* parse_resp_buffer(char **buf) {
    if (!buf || !(*buf) || **buf == '\0') {
        return NULL;
    }

    switch (**buf) {
        case '+':
            return NULL;
        case '-':
            return NULL;
        case ':':
            return NULL;
        case '$':
            // Bulk string
            return parse_bulk_string(buf);
        case '*':
            // Array
            return parse_array(buf);
        default:
            // Skip any invalid characters and try to find the next valid command
            // This helps with robustness when processing multi-command buffers
            (*buf)++;
            return NULL;
    }
}

RESPData* parse_array(char **buf) {
    // Example: *3\r\n$3\r\nfoo\r\n$3\r\nbar\r\n$5\r\nHello\r\n
    // ["foo", "bar", "Hello"]
    char *start = *buf;
    char *end = strchr(start, '\r');
    RESPData *data = malloc(sizeof(RESPData));
    data->type = RESP_ARRAY;

    long count = strtol(start + 1, NULL, 10);

    if (count < 0) {
        data->type = RESP_NULL;
        data->data.array.elements = NULL;
        data->data.array.count = 0;
        *buf = end + 2;
        return data;
    }
    // TODO: Need more handling of cases such as -1, Needing to send actual null

    *buf = end + 2; // Skip the "*3\r\n" part
    
    data->data.array.count = count;
    data->data.array.elements = malloc(count * sizeof(RESPData*));
    
    for (int i = 0; i < count; i++) {
        data->data.array.elements[i] = parse_resp_buffer(buf);

        if (data->data.array.elements[i] == NULL) {
            printf("Error parsing array element %d\n", i);
            // Free the previously allocated elements
            for (int j = 0; j < i; j++) {
                free_resp_data(data->data.array.elements[j]);
                free(data->data.array.elements[j]);
            }
            free(data->data.array.elements);
            free(data);
            return NULL;
        }
    }

    return data;
}

size_t convert_to_resp_array(char *buffer, size_t buffer_size, int count, const char *strings[]) {
    if (buffer == NULL) {
        // Calculate required size without writing
        size_t required_size = snprintf(NULL, 0, "*%d\r\n", count);
        
        for (int i = 0; i < count; i++) {
            const char *str = strings[i];
            
            if (str == NULL) {
                required_size += strlen("$-1\r\n");
            } else {
                int length = strlen(str);
                required_size += snprintf(NULL, 0, "$%d\r\n%s\r\n", length, str);
            }
        }
        
        return required_size;
    }
    
    size_t written = 0;
    int result;
    
    // Add the *<count> prefix to indicate the number of elements
    result = snprintf(buffer, buffer_size, "*%d\r\n", count);
    if (result < 0 || (size_t)result >= buffer_size) {
        return 0;
    }
    written += result;
    
    for (int i = 0; i < count; i++) {
        const char *str = strings[i];
        
        if (str == NULL) {
            const char *null_str = "$-1\r\n";
            size_t null_len = strlen(null_str);
            
            if (written + null_len >= buffer_size) {
                return 0; // Buffer too small
            }
            
            memcpy(buffer + written, null_str, null_len);
            written += null_len;
        } else {
            int length = strlen(str);
            result = snprintf(buffer + written, buffer_size - written, "$%d\r\n", length);
            
            if (result < 0 || written + result >= buffer_size) {
                return 0;
            }
            written += result;
            
            if (written + length >= buffer_size) {
                return 0; 
            }
            memcpy(buffer + written, str, length);
            written += length;
            
            if (written + 2 >= buffer_size) {
                return 0;
            }
            memcpy(buffer + written, "\r\n", 2);
            written += 2;
        }
    }
    
    if (written < buffer_size) {
        buffer[written] = '\0';
    }
    
    return written;
}

char* convert_to_resp_string(const char *str) {
    // $<length>\r\n<data>\r\n
    if (str == NULL) {
        return strdup("$-1\r\n");
    }

    int length = strlen(str);
    char *result = malloc(length + 20); // 20 is arbitrary, just to be safe
    if (!result) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(1);
    }

    snprintf(result, length + 20, "$%d\r\n%s\r\n", length, str);
    return result;
}