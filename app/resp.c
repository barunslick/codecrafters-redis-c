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
    switch (*buf[0]) {
        // case '+':
        //     return parse_simple_string(buf);
        // case '-':
        //     return parse_exit_with_error(buf);
        // case ':':
        //     return parse_integer(buf);
        case '$':
            return parse_bulk_string(buf);
        case '*':
            return parse_array(buf);
        default:
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

char* convert_to_resp_bulk(int count, const char *strings[]) {
    // Allocate a buffer with enough space for the expected output
    // TODO: Make this resizable
    size_t buffer_size = 1024;
    char *result = malloc(buffer_size);
    if (!result) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(1);
    }
    result[0] = '\0'; // Start with an empty string

    // Add the *<count> prefix to indicate the number of elements
    snprintf(result, buffer_size, "*%d\r\n", count);

    for (int i = 0; i < count; i++) {
        const char *str = strings[i];
        char buffer[256];

        if (str == NULL) {
            snprintf(buffer, sizeof(buffer), "$-1\r\n");
        } else {
            int length = strlen(str);
            snprintf(buffer, sizeof(buffer), "$%d\r\n%s\r\n", length, str);
        }
        strcat(result, buffer);
    }

    return result;
}