#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "hashtable.h"

#define INITIAL_CAPACITY 32

uint64_t get_current_epoch_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

ht_table* ht_create() {
    // Allocate space for hash table struct.
	ht_table* table = malloc(sizeof(ht_table));
	if (table == NULL)
		return NULL;

	table->length = 0;
	table->capacity = INITIAL_CAPACITY;

	// Allocate (zero'd) space for entry buckets.
	table->entries = calloc(table->capacity, sizeof(ht_entry));
	if (table->entries == NULL) {
		free(table); // error, free table before we return!
		return NULL;
	}
	return table;
}

void ht_destroy(ht_table* table) {
	for (size_t i = 0; i < table->capacity; i++) {
		if (table->entries[i].key != NULL) {
			free((void*)table->entries[i].key);
			free(table->entries[i].value);  // Free the allocated value
		}
	}

	free(table->entries);
	free(table);
}

#define FNV_OFFSET 14695981039346656037UL
#define FNV_PRIME 1099511628211UL
// Return 64-bit FNV-1a hash for key (NUL-terminated). See description:
// https://en.wikipedia.org/wiki/Fowler–Noll–Vo_hash_function
static uint64_t hash_key(const char* key) {
	uint64_t hash = FNV_OFFSET;
	for (const char* p = key; *p; p++) {
		hash ^= (uint64_t)(unsigned char)(*p);
		hash *= FNV_PRIME;
	}
	return hash;
}

void* ht_get(ht_table* table, const char* key) {
	uint64_t hash = hash_key(key);
	size_t index = (size_t)(hash & (uint64_t)(table->capacity - 1));

	while (table->entries[index].key != NULL) {
		if (strcmp(key, table->entries[index].key) == 0){
			if (table->entries[index].expiry != 0 && table->entries[index].expiry < get_current_epoch_ms()){
				ht_del(table, key);
				return NULL;
			}
			return table->entries[index].value;
		}
		index++;
		if (index >= table->capacity)
			index = 0;
	}
	return NULL;
}

const char* ht_set(ht_table* table, const char* key, void* value, uint64_t expiry) {
	if (table == NULL || key == NULL || value == NULL) {
		return NULL;
	}

	// TODO LATER: If we're at 75% capacity, resize the table.
	if (table->length >= table->capacity)
		return NULL;

	uint64_t hash = hash_key(key);
	size_t index = (size_t)(hash & (uint64_t)(table->capacity - 1));

	// Make a deep copy of the value string
	char* value_copy = strdup((const char*)value);
	if (value_copy == NULL) {
		return NULL;  // Memory allocation failed
	}

	while (table->entries[index].key != NULL) {
		if (strcmp(key, table->entries[index].key) == 0) {
			// Free the old value before replacing it
			free(table->entries[index].value);
			table->entries[index].value = value_copy;
			table->entries[index].expiry = expiry;
			return key;
		}
		index++;
		if (index >= table->capacity)
			index = 0;
	}

	table->entries[index].key = strdup(key);
	if (table->entries[index].key == NULL) {
		free(value_copy);  // Free allocated value if key allocation fails
		return NULL;
	}
	
	table->entries[index].value = value_copy;
	table->entries[index].expiry = expiry;
	table->length++;
	
	return table->entries[index].key;
}

const char * ht_set_with_relative_expiry(ht_table* table, const char* key, void* value, uint64_t expiry) {
	// get absolute time for expiry
	uint64_t expiry_abs = 0;
	if (expiry > 0)
		expiry_abs = expiry + get_current_epoch_ms();
	ht_set(table, key, value, expiry_abs);
}

void ht_del(ht_table* table, const char* key) {
	if (table == NULL || key == NULL) {
		return;
	}

	if (table->length == 0) {
		return;
	}

	uint64_t hash = hash_key(key);
	size_t index = (size_t)(hash & (uint64_t)(table->capacity - 1));

	while (table->entries[index].key != NULL) {
		if (strcmp(key, table->entries[index].key) == 0) {
			free((void*)table->entries[index].key);
			free(table->entries[index].value);  // Free the value we allocated
			table->entries[index].key = NULL;
			table->entries[index].value = NULL;
			table->entries[index].expiry = 0;
			table->length--;
			return;
		}
		index++;
		if (index >= table->capacity)
			index = 0;
	}

	return;
}


const char** ht_get_keys(ht_table* table, size_t* count) {
	if (table == NULL || count == NULL) {
		return NULL;
	}

	*count = table->length;
	const char** keys = malloc(table->length * sizeof(const char*));
	if (keys == NULL) {
		return NULL;
	}

	size_t key_index = 0;
	for (size_t i = 0; i < table->capacity; i++) {
		if (table->entries[i].key != NULL) {
			keys[key_index++] = table->entries[i].key;
		}
	}

	return keys;
}