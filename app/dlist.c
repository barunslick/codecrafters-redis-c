// Create a dynamic linked list
#include <stdio.h>
#include <stdlib.h>

#include "helper.h"
#include "dist.h"



Llist* create_list() {
    Llist* list = (Llist*)malloc(sizeof(Llist));
    if (list == NULL) {
        exit_with_error("Memory allocation failed");
    }
    list->head = list->tail = NULL;
    list->len = 0;
    return list;
}

void free_list(Llist* list) {
    Node* current = list->head;
    while (current != NULL) {
        Node* next = current->next;
        free(current->data);
        free(current);
        current = next;
    }
    free(list);
}

Llist* add_to_list_tail(Llist* list, void* data) {
    Node* new_node;

    if ((new_node = malloc(sizeof(Node))) == NULL)
        return NULL;

    new_node->data = data;

    if (list->len == 0) {
        list->tail = list->head = new_node;
        new_node->prev = new_node->next = NULL;
    } else {
        new_node->prev = list->tail;
        new_node->next = NULL;
        list->tail->next = NULL;
        list->tail = new_node;
    }

    list->len++;

    return list;
}

Llist* add_to_list_head(Llist* list, void* data) {
    Node* new_node;

    if ((new_node = malloc(sizeof(Node))) == NULL)
        return NULL;

    new_node->data = data;

    if (list->len == 0) {
        list->tail = list->head = new_node;
        new_node->prev = new_node->next = NULL;
    } else {
        new_node->prev = NULL;
        new_node->next = list->head;
        list->head->prev = new_node;
        list->head = new_node;
    }

    list->len++;
    return list;
}


void unlink_node(Llist* list, Node* node) {
    if (node->prev != NULL) {
        node->prev->next = node->next;
    } else {
        list->head = node->next;
    }

    if (node->next != NULL) {
        node->next->prev = node->prev;
    }
    else{
        list->tail = node->prev;
    }

    node->next = NULL;
    node->prev = NULL;

    list->len--;
}

void delete_node(Llist* list, Node* node) {
    // This will not free the value for now
    unlink_node(list, node);
    free(node);
}

Node* search_item(Llist* list, void* key){
    // Add support for comparator function later on
    if(!list || list->len ==0)
        return NULL;

    Node* current = list->head;

    while(current != NULL) {
        if (current->data == key) {
            return current;
        }
        current = current->next;
    }

    return NULL;
}