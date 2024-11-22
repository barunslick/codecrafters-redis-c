typedef struct {
	const char* key;
	void* value;
} ht_entry;

typedef struct ht_table {
	size_t capacity;
	size_t length;
	ht_entry* entries;
} ht_table;

ht_table* ht_create();
void ht_destroy(ht_table* table);
void* ht_get(ht_table* table, const char* key);
const char* ht_set(ht_table* table, const char* key, void* value);
void ht_del(ht_table* table, const char* key);
