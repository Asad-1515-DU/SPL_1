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
#include <mutex>
#include <string>
#include <fstream>

using namespace std;  // Move this before any string usage

const int PORT = 8080;
const int TIMEOUT = 3;
const char* DEFAULT_IP = "192.168.0.109";  // Changed from string to char*
const int PACKET_SIZE = 1024;
const int MAX_BUFFER_SIZE = 1024;
const int MAX_RETRIES = 5;
const int SEND_BUFFER_SIZE = 8192;  // Larger buffer for better performance
const int MIN_TIMEOUT_MS = 100;     // Minimum timeout in milliseconds
const int MAX_TIMEOUT_MS = 5000;    // Maximum timeout in milliseconds

atomic<bool> is_running{true};

// Utility functions
void handle_error(const string& msg) {
    cerr << "[ERROR] " << msg << ": " << strerror(errno) << endl;
    exit(1);
}

class AdaptiveTimeout {
    int current_ms = 1000;
    const int min_ms, max_ms;
public:
    AdaptiveTimeout(int min = MIN_TIMEOUT_MS, int max = MAX_TIMEOUT_MS) 
        : min_ms(min), max_ms(max) {}
    
    void decrease() { current_ms = std::max(current_ms / 2, min_ms); }
    void increase() { current_ms = std::min(current_ms * 2, max_ms); }
    int get() const { return current_ms; }
};

class PacketBuffer {
    vector<string> packets;
    mutex mtx;
public:
    explicit PacketBuffer(size_t size) : packets(size) {}
    
    void store(int seq_num, const string& packet) {
        lock_guard<mutex> lock(mtx);
        packets[seq_num] = packet;
    }
    
    string get(int seq_num) {
        lock_guard<mutex> lock(mtx);
        return packets[seq_num];
    }
};

int create_udp_socket() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        handle_error("Socket creation failed");
    }
    
    // Set socket options
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        handle_error("setsockopt(SO_REUSEADDR) failed");
    }
    
    // Add send buffer optimization
    int send_buff_size = SEND_BUFFER_SIZE;
    if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &send_buff_size, sizeof(send_buff_size)) < 0) {
        handle_error("setsockopt(SO_SNDBUF) failed");
    }
    
    // Enable keep-alive
    int keepalive = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) < 0) {
        handle_error("setsockopt(SO_KEEPALIVE) failed");
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
string create_packet_with_message(int seq_num, const string& message = "test") {
    int checksum = 0;
    for(char c : message) {
        checksum += c;
    }
    return to_string(seq_num) + ":" + message + ":" + to_string(checksum);
}

// Replace existing create_packet function
string create_packet(int seq_num) {
    return create_packet_with_message(seq_num);
}

bool can_send(int next_seq_num, int base, int window_size) {
    return next_seq_num < base + window_size;
}

enum Protocol {
    STOP_AND_WAIT,
    GO_BACK_N,
    SELECTIVE_REPEAT
};

void log_statistics(const TransmissionStats& stats, int total_packets, int window_size, const vector<bool>& ack_received, const vector<int>& lost_packets) {
    ofstream stat_file("stat.txt");
    if (!stat_file.is_open()) {
        cerr << "[ERROR] Failed to open stat.txt for writing\n";
        return;
    }

    stat_file << "Total Packets: " << total_packets << "\n";
    stat_file << "Window Size: " << window_size << "\n";
    stat_file << "Packets Sent: " << stats.packets_sent << "\n";
    stat_file << "Packets Lost: " << stats.packets_lost << "\n";
    stat_file << "Retransmissions: " << stats.retransmissions << "\n";
    stat_file << "ACK Received: ";
    for (bool ack : ack_received) {
        stat_file << ack << " ";
    }
    stat_file << "\nLost Packets: ";
    for (int lost : lost_packets) {
        stat_file << lost << " ";
    }
    stat_file << "\n";
    stat_file.close();
}

void stop_and_wait_sender(const string& receiver_ip, int total_packets) {
    int sock = create_udp_socket();
    configure_socket_timeout(sock, TIMEOUT);
    
    TransmissionStats stats = {0, 0, 0};
    const int WINDOW_SIZE = 1;  // Stop-and-Wait uses window size of 1
    int base = 0, next_seq_num = 0;
    vector<bool> ack_received(total_packets, false);
    vector<int> lost_packets;
    
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, receiver_ip.c_str(), &server_addr.sin_addr) <= 0) {
        handle_error("Invalid receiver IP address");
    }

    AdaptiveTimeout timeout;
    PacketBuffer packet_buffer(total_packets);

    auto timeout_handler = [&]() {
        while(is_running && base < total_packets) {
            this_thread::sleep_for(chrono::milliseconds(timeout.get()));
            if (base < next_seq_num && !ack_received[base]) {
                string packet = packet_buffer.get(base);
                if (sendto(sock, packet.c_str(), packet.size(), 0, 
                          (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
                    timeout.increase();
                } else {
                    stats.retransmissions++;
                }
            }
        }
    };

    thread timeout_thread(timeout_handler);

    while (base < total_packets) {
        // Send packet if within window (window size = 1)
        if (can_send(next_seq_num, base, WINDOW_SIZE) && next_seq_num < total_packets) {
            string packet = create_packet(next_seq_num);
            packet_buffer.store(next_seq_num, packet);
            
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
                lost_packets.push_back(next_seq_num);
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
    log_statistics(stats, total_packets, 1, ack_received, lost_packets);
    cout << "[Sender] Transmission completed\n";
}

void selective_repeat_sender(const string& receiver_ip, int total_packets, int window_size) {
    int sock = create_udp_socket();
    configure_socket_timeout(sock, TIMEOUT);
    
    TransmissionStats stats = {0, 0, 0};
    int base = 0, next_seq_num = 0;
    vector<bool> ack_received(total_packets, false);
    vector<int> lost_packets;
    
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, receiver_ip.c_str(), &server_addr.sin_addr) <= 0) {
        handle_error("Invalid receiver IP address");
    }

    AdaptiveTimeout timeout;
    PacketBuffer packet_buffer(total_packets);

    auto timeout_handler = [&]() {
        while(is_running) {
            this_thread::sleep_for(chrono::milliseconds(timeout.get()));
            for (int i = base; i < min(next_seq_num, base + window_size); i++) {
                if (!ack_received[i]) {
                    cout << "[Sender] Timeout. Resending packet " << i << "\n";
                    string packet = packet_buffer.get(i);
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
            packet_buffer.store(next_seq_num, packet);
            
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
                lost_packets.push_back(next_seq_num);
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
    log_statistics(stats, total_packets, window_size, ack_received, lost_packets);
    cout << "[Sender] Transmission completed\n";
}

void sender(Protocol protocol, const string& receiver_ip, int w_size, int t_packets) {
    if (protocol == STOP_AND_WAIT) {
        stop_and_wait_sender(receiver_ip, t_packets);
    } else if (protocol == GO_BACK_N) {
        // Original Go-Back-N implementation
        int sock = create_udp_socket();
        configure_socket_timeout(sock, TIMEOUT);
        
        TransmissionStats stats = {0, 0, 0};
        
        int WINDOW_SIZE = w_size;
        int TOTAL_PACKETS = t_packets;
        int base = 0, next_seq_num = 0;
        vector<bool> ack_received(TOTAL_PACKETS, false);
        vector<int> lost_packets;

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(PORT);
        if (inet_pton(AF_INET, receiver_ip.c_str(), &server_addr.sin_addr) <= 0) {
            handle_error("Invalid receiver IP address");
        }

        AdaptiveTimeout timeout;
        PacketBuffer packet_buffer(TOTAL_PACKETS);

        auto timeout_handler = [&]() {
            while(is_running) {
                this_thread::sleep_for(chrono::milliseconds(timeout.get()));
                if (base < next_seq_num) {
                    cout << "[Sender] Timeout. Resending from " << base << " to " << next_seq_num-1 << "\n";
                    for (int i = base; i < next_seq_num; i++) {
                        if (!ack_received[i]) {
                            string packet = packet_buffer.get(i);
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
                packet_buffer.store(next_seq_num, packet);
                
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
                    lost_packets.push_back(next_seq_num);
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
        log_statistics(stats, t_packets, w_size, ack_received, lost_packets);
        cout << "[Sender] Transmission completed\n";
    } else {
        selective_repeat_sender(receiver_ip, t_packets, w_size);
    }
}

int main(int argc, char* argv[]) {
    // Add better IP handling
    string receiver_ip;
    if (argc < 2) {
        cout << "Usage: " << argv[0] << " <receiver_ip>\n";
        cout << "Enter receiver IP address: ";
        cin >> receiver_ip;
    } else {
        receiver_ip = argv[1];
    }

    // Verify IP address immediately
    struct sockaddr_in sa;
    if (inet_pton(AF_INET, receiver_ip.c_str(), &(sa.sin_addr)) != 1) {
        cerr << "Error: Invalid IP address format\n";
        return 1;
    }

    cout << "Connecting to receiver at: " << receiver_ip << ":" << PORT << endl;

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
    
    // Validate protocol choice
    if (protocol_choice < 1 || protocol_choice > 3) {
        cerr << "Invalid protocol choice. Defaulting to Stop-and-Wait.\n";
        protocol_choice = 1;
    }

    // Validate window size
    if (WINDOW_SIZE < 1) {
        cerr << "Invalid window size. Setting to 1.\n";
        WINDOW_SIZE = 1;
    }

    // Validate total packets
    if (TOTAL_PACKETS < 1) {
        cerr << "Invalid packet count. Setting to 1.\n";
        TOTAL_PACKETS = 1;
    }

    Protocol selected_protocol;
    switch(protocol_choice) {
        case 1: selected_protocol = STOP_AND_WAIT; break;
        case 2: selected_protocol = GO_BACK_N; break;
        case 3: selected_protocol = SELECTIVE_REPEAT; break;
        default: selected_protocol = STOP_AND_WAIT;
    }
    
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, receiver_ip.c_str(), &server_addr.sin_addr);
    
    sender(selected_protocol, receiver_ip, WINDOW_SIZE, TOTAL_PACKETS);
    
    return 0;
}
