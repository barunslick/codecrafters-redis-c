#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "hashtable.h"

#define INITIAL_CAPACITY 32

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
		free((void*)table->entries[i].key);
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
		if (strcmp(key, table->entries[index].key) == 0)
			return table->entries[index].value;
		index++;
		if (index >= table->capacity)
			index = 0;
	}
	return NULL;
}

const char* ht_set(ht_table* table, const char* key, void* value) {
	if (table == NULL || key == NULL || value == NULL) {
		return NULL;
	}
	
	// TODO LATER: If we're at 75% capacity, resize the table.
	if (table->length >= table->capacity)
		return NULL;

	uint64_t hash = hash_key(key);
	size_t index = (size_t)(hash & (uint64_t)(table->capacity - 1));

	while (table->entries[index].key != NULL) {
		if (strcmp(key, table->entries[index].key) == 0) {
			table->entries[index].value = value;
			return key;
		}
		index++;
		if (index >= table->capacity)
			index = 0;
	}

	table->entries[index].key = strdup(key);
	table->entries[index].value = value;
	table->length++;
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
			table->entries[index].key = NULL;
			table->entries[index].value = NULL;
			table->length--;
			return;
		}
		index++;
		if (index >= table->capacity)
			index = 0;
	}

	return;
}
