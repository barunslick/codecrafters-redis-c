typedef struct {
	const char* key;
	void* value;
	uint64_t expiry;
} ht_entry;

typedef struct ht_table {
	size_t capacity;
	size_t length;
	ht_entry* entries;
} ht_table;

ht_table* ht_create();
void ht_destroy(ht_table* table);
void* ht_get(ht_table* table, const char* key);
const char* ht_set(ht_table* table, const char* key, void* value, uint64_t expiry);
void ht_del(ht_table* table, const char* key);
const char** ht_get_keys(ht_table* table, size_t* count);
