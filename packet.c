// Packet structure
#include<stdio.h>
struct Packet {
    int sequence_number;
    char data;
    int acked; // 0 = not acknowledged, 1 = acknowledged
    struct Packet* next;
};

// Function to create a new packet
struct Packet* create_packet(int seq, char data) {
    struct Packet* new_packet = (struct Packet*)malloc(sizeof(struct Packet));
    new_packet->sequence_number = seq;
    new_packet->data = data;
    new_packet->acked = 0;
    new_packet->next = NULL;
    return new_packet;
}

// Function to append a packet to the list
void append_packet(struct Packet** head, int seq, char data) {
    struct Packet* new_packet = create_packet(seq, data);
    if (*head == NULL) {
        *head = new_packet;
        return;
    }
    struct Packet* temp = *head;
    while (temp->next != NULL) {
        temp = temp->next;
    }
    temp->next = new_packet;
}

// Function to delete a packet from the list
void delete_packet(struct Packet** head, int seq) {
    struct Packet* temp = *head;
    struct Packet* prev = NULL;
    while (temp != NULL && temp->sequence_number != seq) {
        prev = temp;
        temp = temp->next;
    }
    if (temp == NULL) return; // Not found
    if (prev == NULL) {
        *head = temp->next;
    } else {
        prev->next = temp->next;
    }
    free(temp);
}
