#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <random>
#include <atomic>

const int PORT = 8080;
const int TIMEOUT = 3;
using namespace std;

atomic<bool> is_running{true};

// Utility functions
int create_udp_socket() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        exit(1);
    }
    return sock;
}

void configure_socket_timeout(int sock, int timeout_sec) {
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("Setsockopt failed");
        exit(1);
    }
}

bool simulate_packet_loss() {
    static random_device rd;
    static mt19937 gen(rd());
    static uniform_real_distribution<> dis(0, 1);
    return dis(gen) < 0.1; // 10% packet loss rate
}

string create_packet_with_checksum(int seq_num, const string& data) {
    int checksum = 0;
    for (char c : data) {
        checksum += c;
    }
    return to_string(seq_num) + ":" + data + ":" + to_string(checksum);
}

struct TransmissionStats {
    int packets_sent;
    int packets_lost;
    int retransmissions;
    
    void print() {
        cout << "\n=== Transmission Statistics ===\n"
             << "Packets sent: " << packets_sent << "\n"
             << "Packets lost: " << packets_lost << "\n"
             << "Retransmissions: " << retransmissions << "\n";
    }
};

// Helper functions for packet management
string create_packet(int seq_num) {
    return to_string(seq_num);
}

bool can_send(int next_seq_num, int base, int window_size) {
    return next_seq_num < base + window_size;
}

enum Protocol {
    STOP_AND_WAIT,
    GO_BACK_N,
    SELECTIVE_REPEAT
};

void stop_and_wait_sender(int total_packets) {
    int sock = create_udp_socket();
    configure_socket_timeout(sock, TIMEOUT);
    
    TransmissionStats stats = {0, 0, 0};
    const int WINDOW_SIZE = 1;  // Stop-and-Wait uses window size of 1
    int base = 0, next_seq_num = 0;
    vector<bool> ack_received(total_packets, false);
    
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    auto timeout_handler = [&]() {
        while(is_running && base < total_packets) {
            this_thread::sleep_for(chrono::seconds(TIMEOUT));
            if (base < next_seq_num && !ack_received[base]) {
                cout << "[Sender] Timeout. Resending packet " << base << "\n";
                string packet = create_packet(base);
                sendto(sock, packet.c_str(), packet.size(), 0, 
                       (sockaddr*)&server_addr, sizeof(server_addr));
                stats.retransmissions++;
            }
        }
    };

    thread timeout_thread(timeout_handler);

    while (base < total_packets) {
        // Send packet if within window (window size = 1)
        if (can_send(next_seq_num, base, WINDOW_SIZE) && next_seq_num < total_packets) {
            string packet = create_packet(next_seq_num);
            
            if (!simulate_packet_loss()) {
                if (sendto(sock, packet.c_str(), packet.size(), 0, 
                          (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
                    cerr << "[ERROR] Failed to send packet " << next_seq_num << "\n";
                    continue;
                }
                cout << "[SENT] Packet " << next_seq_num << " | Window base: " << base << "\n";
                stats.packets_sent++;
            } else {
                cout << "[LOST] Packet " << next_seq_num << " lost in transmission\n";
                stats.packets_lost++;
            }
            next_seq_num++;
        }

        // Handle ACK
        char buffer[1024] = {0};
        sockaddr_in recv_addr{};
        socklen_t addr_len = sizeof(recv_addr);
        
        int bytes_received = recvfrom(sock, buffer, sizeof(buffer), 0, 
                                    (sockaddr*)&recv_addr, &addr_len);

        if (bytes_received > 0) {
            try {
                int ack = stoi(buffer);
                cout << "[Sender] ACK received: " << ack << "\n";
                if (ack == base) {
                    ack_received[ack] = true;
                    base++;  // Slide the window
                }
            } catch (const exception& e) {
                cerr << "[Sender] Invalid ACK received\n";
            }
        }
        
        this_thread::sleep_for(chrono::milliseconds(100));
    }

    is_running = false;
    timeout_thread.join();
    close(sock);
    stats.print();
    cout << "[Sender] Transmission completed\n";
}

void selective_repeat_sender(int total_packets, int window_size) {
    int sock = create_udp_socket();
    configure_socket_timeout(sock, TIMEOUT);
    
    TransmissionStats stats = {0, 0, 0};
    int base = 0, next_seq_num = 0;
    vector<bool> ack_received(total_packets, false);
    
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    auto timeout_handler = [&]() {
        while(is_running) {
            this_thread::sleep_for(chrono::seconds(TIMEOUT));
            for (int i = base; i < min(next_seq_num, base + window_size); i++) {
                if (!ack_received[i]) {
                    cout << "[Sender] Timeout. Resending packet " << i << "\n";
                    string packet = create_packet(i);
                    sendto(sock, packet.c_str(), packet.size(), 0, 
                           (sockaddr*)&server_addr, sizeof(server_addr));
                    stats.retransmissions++;
                }
            }
        }
    };

    thread timeout_thread(timeout_handler);

    while (base < total_packets) {
        if (can_send(next_seq_num, base, window_size) && next_seq_num < total_packets) {
            string packet = create_packet(next_seq_num);
            
            if (!simulate_packet_loss()) {
                if (sendto(sock, packet.c_str(), packet.size(), 0, 
                          (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
                    cerr << "[ERROR] Failed to send packet " << next_seq_num << "\n";
                    continue;
                }
                cout << "[SENT] Packet " << next_seq_num << " | Window base: " << base << "\n";
                stats.packets_sent++;
            } else {
                cout << "[LOST] Packet " << next_seq_num << " lost in transmission\n";
                stats.packets_lost++;
            }
            next_seq_num++;
        }

        char buffer[1024] = {0};
        sockaddr_in recv_addr{};
        socklen_t addr_len = sizeof(recv_addr);
        
        int bytes_received = recvfrom(sock, buffer, sizeof(buffer), 0, 
                                    (sockaddr*)&recv_addr, &addr_len);

        if (bytes_received > 0) {
            try {
                int ack = stoi(buffer);
                cout << "[Sender] ACK received: " << ack << "\n";
                ack_received[ack] = true;
                
                // Move base if possible
                while (base < total_packets && ack_received[base]) {
                    base++;
                }
            } catch (const exception& e) {
                cerr << "[Sender] Invalid ACK received\n";
            }
        }
        
        this_thread::sleep_for(chrono::milliseconds(100));
    }

    is_running = false;
    timeout_thread.join();
    close(sock);
    stats.print();
    cout << "[Sender] Transmission completed\n";
}

void sender(Protocol protocol, int w_size, int t_packets) {
    if (protocol == STOP_AND_WAIT) {
        stop_and_wait_sender(t_packets);
    } else if (protocol == GO_BACK_N) {
        // Original Go-Back-N implementation
        int sock = create_udp_socket();
        configure_socket_timeout(sock, TIMEOUT);
        
        TransmissionStats stats = {0, 0, 0};
        
        int WINDOW_SIZE = w_size;
        int TOTAL_PACKETS = t_packets;
        int base = 0, next_seq_num = 0;
        vector<bool> ack_received(TOTAL_PACKETS, false);

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(PORT);
        inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

        auto timeout_handler = [&]() {
            while(is_running) {
                this_thread::sleep_for(chrono::seconds(TIMEOUT));
                if (base < next_seq_num) {
                    cout << "[Sender] Timeout. Resending from " << base << " to " << next_seq_num-1 << "\n";
                    for (int i = base; i < next_seq_num; i++) {
                        if (!ack_received[i]) {
                            string packet = create_packet(i);
                            sendto(sock, packet.c_str(), packet.size(), 0, 
                                   (sockaddr*)&server_addr, sizeof(server_addr));
                            cout << "[Sender] Resent: " << i << "\n";
                        }
                    }
                }
            }
        };

        thread timeout_thread(timeout_handler);

        while (base < TOTAL_PACKETS) {
            // Send packets within window
            while (can_send(next_seq_num, base, WINDOW_SIZE) && next_seq_num < TOTAL_PACKETS) {
                string packet = create_packet(next_seq_num);
                
                if (!simulate_packet_loss()) {
                    if (sendto(sock, packet.c_str(), packet.size(), 0, 
                              (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
                        cerr << "[ERROR] Failed to send packet " << next_seq_num << "\n";
                        continue;
                    }
                    cout << "[SENT] Packet " << next_seq_num << " | Window base: " << base << "\n";
                    stats.packets_sent++;
                } else {
                    cout << "[LOST] Packet " << next_seq_num << " lost in transmission\n";
                    stats.packets_lost++;
                }
                
                next_seq_num++;
                this_thread::sleep_for(chrono::milliseconds(100));
            }

            // Handle ACKs
            char buffer[1024] = {0};
            sockaddr_in recv_addr{};
            socklen_t addr_len = sizeof(recv_addr);
            
            int bytes_received = recvfrom(sock, buffer, sizeof(buffer), 0, 
                                        (sockaddr*)&recv_addr, &addr_len);

            if (bytes_received > 0) {
                try {
                    int ack = stoi(buffer);
                    cout << "[Sender] ACK received: " << ack << "\n";
                    if (ack >= base) {
                        ack_received[ack] = true;
                        while (base < TOTAL_PACKETS && ack_received[base]) {
                            base++;
                        }
                    }
                } catch (const exception& e) {
                    cerr << "[Sender] Invalid ACK received\n";
                }
            }
        }
    stats.retransmissions = t_packets - stats.packets_sent;
        is_running = false;
        timeout_thread.join();
        close(sock);
        stats.print();
        cout << "[Sender] Transmission completed\n";
    } else {
        selective_repeat_sender(t_packets, w_size);
    }
}

int main() {
    int protocol_choice, WINDOW_SIZE = 1, TOTAL_PACKETS;
    
    cout << "Select ARQ Protocol:\n";
    cout << "1. Stop-and-Wait\n";
    cout << "2. Go-Back-N\n";
    cout << "3. Selective Repeat\n";
    cout << "Enter choice (1-3): ";
    cin >> protocol_choice;
    
    cout << "Enter Number of total packets: ";
    cin >> TOTAL_PACKETS;
    
    if (protocol_choice > 1) {
        cout << "Enter Window Size: ";
        cin >> WINDOW_SIZE;
    }
    
    Protocol selected_protocol;
    switch(protocol_choice) {
        case 1: selected_protocol = STOP_AND_WAIT; break;
        case 2: selected_protocol = GO_BACK_N; break;
        case 3: selected_protocol = SELECTIVE_REPEAT; break;
        default: selected_protocol = STOP_AND_WAIT;
    }
    
    sender(selected_protocol, WINDOW_SIZE, TOTAL_PACKETS);
    
    return 0;
}

