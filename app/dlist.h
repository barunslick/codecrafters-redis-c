#ifndef DLIST_H
#define DLIST_H

typedef struct Node {
    struct Node* prev;
    struct Node* next;
    void* data;
} Node;

typedef struct {
    Node* head;
    Node* tail;
    unsigned long len;
} Llist;

Llist* create_list();
void free_list(Llist* list);
Llist* add_to_list_tail(Llist* list, void* data);
Llist* add_to_list_head(Llist* list, void* data);
void unlink_node(Llist* list, Node* node);
void delete_node(Llist* list, Node* node);
Node* search_item(Llist* list, void* key);

#endif /* DLIST_H */