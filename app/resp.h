#ifndef RESP_H
#define RESP_H

typedef enum {
    RESP_INVALID,
    RESP_SIMPLE_STRING,
    RESP_ERROR,
    RESP_INTEGER,
    RESP_BULK_STRING,
    RESP_ARRAY,
    RESP_NULL
} RESPType;

typedef struct RESPData {
    RESPType type;
    union {
        char *str;
        char *error;
        long long integer;
        struct {
            struct RESPData **elements;
            size_t count;
        } array;
    } data;
} RESPData;

// Parser functions
RESPData* parse_resp_buffer(char** buf);
RESPData* parse_bulk_string(char** buf);
RESPData* parse_array(char** buf);
void free_resp_data(RESPData* data);

// Encoder functions
char* convert_to_resp_bulk(int count, const char *strings[]);

#endif // RESP_H