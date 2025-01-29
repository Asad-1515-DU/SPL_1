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

void receiver() {
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

    int expected_seq_num = 0;
    vector<bool> received_packets(1000, false);

    cout << "[Receiver] Started. Waiting for packets...\n";

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

                if (seq_num == expected_seq_num) {
                    send_ack(sock, seq_num, client_addr);
                    received_packets[seq_num] = true;
                    
                    while (received_packets[expected_seq_num]) {
                        expected_seq_num++;
                    }
                } else if (seq_num > expected_seq_num) {
                    cout << "[Receiver] Out of order packet " << seq_num << 
                            ". Expected " << expected_seq_num << "\n";
                    received_packets[seq_num] = true;
                    send_ack(sock, seq_num, client_addr);
                }
            } catch (const exception& e) {
                cerr << "[Receiver] Invalid packet received\n";
            }
        }
    }

    close(sock);
    //stats.print();
}

int main() {
    receiver();
    return 0;
}

