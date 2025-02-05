#include <iostream>
#include <thread>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sstream>
#include <vector>


const int PORT = 8080;
using namespace std;

enum Protocol {
    STOP_AND_WAIT,
    GO_BACK_N,
    SELECTIVE_REPEAT
};

struct ReceiverStats {
    int packets_received;
    int corrupted_packets;
    int out_of_order;
    
    void print() {
        cout << "\n=== Receiver Statistics ===\n"
             << "Packets received: " << packets_received << "\n"
             << "Corrupted packets: " << corrupted_packets << "\n"
             << "Out of order packets: " << out_of_order << "\n";
    }
};

bool verify_checksum(const string& packet, int& seq_num) {
    stringstream ss(packet);
    string seq_str, data, checksum_str;
    getline(ss, seq_str, ':');
    getline(ss, data, ':');
    getline(ss, checksum_str, ':');
    
    int received_checksum = stoi(checksum_str);
    int calculated_checksum = 0;
    for (char c : data) {
        calculated_checksum += c;
    }
    
    seq_num = stoi(seq_str);
    return received_checksum == calculated_checksum;
}

void send_ack(int sock, int seq_num, sockaddr_in& client_addr) {
    string ack = to_string(seq_num);
    socklen_t addr_len = sizeof(client_addr);
    sendto(sock, ack.c_str(), ack.length(), 0, 
           (sockaddr*)&client_addr, addr_len);
    cout << "[Receiver] Sent ACK: " << seq_num << "\n";
}

void stop_and_wait_receiver() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(sock);
        return;
    }

    ReceiverStats stats = {0, 0, 0};
    int expected_seq_num = 0;
    cout << "[Receiver] Started in Stop-and-Wait mode. Waiting for packets...\n";

    while (true) {
        char buffer[1024] = {0};
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        
        int bytes_received = recvfrom(sock, buffer, sizeof(buffer), 0, 
                                    (sockaddr*)&client_addr, &addr_len);

        if (bytes_received > 0) {
            try {
                int seq_num = stoi(buffer);
                cout << "[Receiver] Received packet " << seq_num << "\n";
                stats.packets_received++;

                if (seq_num == expected_seq_num) {
                    send_ack(sock, seq_num, client_addr);
                    expected_seq_num++;
                } else {
                    stats.out_of_order++;
                    cout << "[Receiver] Out of order packet. Expected " 
                         << expected_seq_num << ", got " << seq_num << "\n";
                    send_ack(sock, expected_seq_num - 1, client_addr);
                }
            } catch (const exception& e) {
                stats.corrupted_packets++;
                cerr << "[Receiver] Invalid packet received\n";
            }
        }
    }

    close(sock);
    stats.print();
}

void go_back_n_receiver() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(sock);
        return;
    }

    ReceiverStats stats = {0, 0, 0};
    int expected_seq_num = 0;
    vector<bool> received_packets(1000, false);

    cout << "[Receiver] Started in Go-Back-N mode. Waiting for packets...\n";

    while (true) {
        char buffer[1024] = {0};
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        
        int bytes_received = recvfrom(sock, buffer, sizeof(buffer), 0, 
                                    (sockaddr*)&client_addr, &addr_len);

        if (bytes_received > 0) {
            try {
                int seq_num = stoi(buffer);
                cout << "[Receiver] Received packet " << seq_num << "\n";
                stats.packets_received++;

                if (seq_num == expected_seq_num) {
                    send_ack(sock, seq_num, client_addr);
                    received_packets[seq_num] = true;
                    
                    while (received_packets[expected_seq_num]) {
                        expected_seq_num++;
                    }
                } else {
                    stats.out_of_order++;
                    cout << "[Receiver] Out of order packet. Expected " 
                         << expected_seq_num << ", got " << seq_num << "\n";
                    if (seq_num > expected_seq_num) {
                        send_ack(sock, expected_seq_num - 1, client_addr);
                    }
                }
            } catch (const exception& e) {
                stats.corrupted_packets++;
                cerr << "[Receiver] Invalid packet received\n";
            }
        }
    }

    close(sock);
    stats.print();
}

void selective_repeat_receiver() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(sock);
        return;
    }

    ReceiverStats stats = {0, 0, 0};
    int expected_seq_num = 0;
    vector<bool> received_packets(1000, false);
    vector<string> packet_buffer(1000);

    cout << "[Receiver] Started in Selective Repeat mode. Waiting for packets...\n";

    while (true) {
        char buffer[1024] = {0};
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        
        int bytes_received = recvfrom(sock, buffer, sizeof(buffer), 0, 
                                    (sockaddr*)&client_addr, &addr_len);

        if (bytes_received > 0) {
            try {
                int seq_num = stoi(buffer);
                cout << "[Receiver] Received packet " << seq_num << "\n";
                stats.packets_received++;

                // Accept and buffer any packet within window
                if (seq_num >= expected_seq_num) {
                    received_packets[seq_num] = true;
                    packet_buffer[seq_num] = string(buffer, bytes_received);
                    send_ack(sock, seq_num, client_addr);
                    
                    // Move window if possible
                    while (received_packets[expected_seq_num]) {
                        cout << "[Receiver] Delivering packet " << expected_seq_num << "\n";
                        expected_seq_num++;
                    }
                } else {
                    stats.out_of_order++;
                    cout << "[Receiver] Out of order packet " << seq_num << "\n";
                    send_ack(sock, seq_num, client_addr);
                }
            } catch (const exception& e) {
                stats.corrupted_packets++;
                cerr << "[Receiver] Invalid packet received\n";
            }
        }
    }

    close(sock);
    stats.print();
}

void receiver(Protocol protocol) {
    if (protocol == STOP_AND_WAIT) {
        stop_and_wait_receiver();
    } else if (protocol == GO_BACK_N) {
        go_back_n_receiver();
    } else {
        selective_repeat_receiver();
    }
}

int main() {
    int protocol_choice;
    cout << "Select ARQ Protocol:\n";
    cout << "1. Stop-and-Wait\n";
    cout << "2. Go-Back-N\n";
    cout << "3. Selective Repeat\n";
    cout << "Enter choice (1-3): ";
    cin >> protocol_choice;
    
    Protocol selected_protocol;
    switch(protocol_choice) {
        case 1: selected_protocol = STOP_AND_WAIT; break;
        case 2: selected_protocol = GO_BACK_N; break;
        case 3: selected_protocol = SELECTIVE_REPEAT; break;
        default: selected_protocol = STOP_AND_WAIT;
    }
    
    receiver(selected_protocol);
    return 0;
}

