#include <iostream>
#include <thread>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sstream>
#include <vector>
#include <signal.h>
#include <deque>
#include <mutex>
#include <condition_variable>

volatile sig_atomic_t running = 1;

void signal_handler(int signum) {
    running = 0;
}

const int PORT = 8080;
const int MAX_BUFFER_SIZE = 1024;
const int MAX_PACKETS = 10000;
const int TIMEOUT_SECONDS = 10;
const int RECV_BUFFER_SIZE = 8192;
const int MAX_QUEUE_SIZE = 1000;
const char* LISTEN_IP = "192.168.0.109";
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
    size_t total_bytes_received;
    
    ReceiverStats() : packets_received(0), corrupted_packets(0), 
                      out_of_order(0), total_bytes_received(0) {}
    
    void print() {
        cout << "\n=== Receiver Statistics ===\n"
             << "Packets received: " << packets_received << "\n"
             << "Corrupted packets: " << corrupted_packets << "\n"
             << "Out of order packets: " << out_of_order << "\n"
             << "Total bytes received: " << total_bytes_received << "\n";
    }
};

void handle_error(const string& msg) {
    cerr << "[ERROR] " << msg << ": " << strerror(errno) << endl;
    exit(1);
}

bool validate_packet(const string& packet, int& seq_num, string& data) {
    try {
        size_t first_colon = packet.find(':');
        size_t last_colon = packet.rfind(':');
        
        if (first_colon == string::npos || last_colon == string::npos || first_colon == last_colon) {
            return false;
        }

        seq_num = stoi(packet.substr(0, first_colon));
        data = packet.substr(first_colon + 1, last_colon - first_colon - 1);
        int received_checksum = stoi(packet.substr(last_colon + 1));
        
        int calculated_checksum = 0;
        for(char c : data) {
            calculated_checksum += c;
        }
        
        return received_checksum == calculated_checksum;
    } catch (...) {
        return false;
    }
}

// Add network interface detection
void print_available_interfaces() {
    system("ip addr show | grep 'inet '");
    cout << "\nAbove are your available network interfaces.\n";
}

int create_receiver_socket() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        handle_error("Socket creation failed");
    }
    
    // Allow broadcast
    int broadcast = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
        handle_error("setsockopt(SO_BROADCAST) failed");
    }
    
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        handle_error("setsockopt(SO_REUSEADDR) failed");
    }
    
    struct timeval tv;
    tv.tv_sec = TIMEOUT_SECONDS;
    tv.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        handle_error("setsockopt(SO_RCVTIMEO) failed");
    }
    
    int recv_buff_size = RECV_BUFFER_SIZE;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recv_buff_size, sizeof(recv_buff_size)) < 0) {
        handle_error("setsockopt(SO_RCVBUF) failed");
    }
    
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;  // Listen on all interfaces

    if (bind(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        handle_error("Bind failed");
    }
    
    cout << "[Receiver] Listening on port " << PORT << " (all interfaces)\n";
    return sock;
}

void send_ack(int sock, int seq_num, sockaddr_in& client_addr) {
    string ack = to_string(seq_num);
    socklen_t addr_len = sizeof(client_addr);
    sendto(sock, ack.c_str(), ack.length(), 0, 
           (sockaddr*)&client_addr, addr_len);
    cout << "[Receiver] Sent ACK: " << seq_num << "\n";
}

pair<int, string> extract_packet_data(const string& packet) {
    size_t pos = packet.find(":");
    if (pos == string::npos) {
        throw runtime_error("Invalid packet format");
    }
    int seq_num = stoi(packet.substr(0, pos));
    string data = packet.substr(pos + 1);
    return {seq_num, data};
}

void process_received_data(const string& data) {
    // No longer print messages
    return;
}

class PacketQueue {
    deque<pair<int, string>> packets;
    mutex mtx;
    condition_variable cv;
    size_t max_size;
public:
    explicit PacketQueue(size_t size = MAX_QUEUE_SIZE) : max_size(size) {}
    
    void push(int seq_num, string data) {
        unique_lock<mutex> lock(mtx);
        cv.wait(lock, [this]{ return packets.size() < max_size; });
        packets.emplace_back(seq_num, move(data));
        cv.notify_one();
    }
    
    pair<int, string> pop() {
        unique_lock<mutex> lock(mtx);
        cv.wait(lock, [this]{ return !packets.empty() || !::running; });
        if (!::running) throw runtime_error("Shutdown requested");
        auto packet = move(packets.front());
        packets.pop_front();
        cv.notify_one();
        return packet;
    }
};

void packet_processor(PacketQueue& queue, ReceiverStats& stats) {
    while(running) {
        try {
            auto [seq_num, data] = queue.pop();
            process_received_data(data);
            stats.total_bytes_received += data.length();
        } catch(const exception& e) {
            if(running) cerr << "Processor error: " << e.what() << endl;
        }
    }
}

void stop_and_wait_receiver() {
    int sock = create_receiver_socket();
    
    ReceiverStats stats;
    int expected_seq_num = 0;
    int timeout_count = 0;
    const int MAX_TIMEOUTS = 5;
    cout << "[Receiver] Started in Stop-and-Wait mode. Waiting for packets...\n";

    PacketQueue packet_queue;
    thread processor(packet_processor, ref(packet_queue), ref(stats));
    
    while (timeout_count < MAX_TIMEOUTS && running) {
        char buffer[MAX_BUFFER_SIZE] = {0};
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        
        int bytes_received = recvfrom(sock, buffer, sizeof(buffer), 0, 
                                    (sockaddr*)&client_addr, &addr_len);

        if (bytes_received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                timeout_count++;
                cout << "[Receiver] Timeout " << timeout_count << "/" << MAX_TIMEOUTS << endl;
                continue;
            }
            handle_error("recvfrom failed");
        }

        timeout_count = 0;

        string packet(buffer, bytes_received);
        int seq_num;
        string data;
        if (validate_packet(packet, seq_num, data)) {
            cout << "[Receiver] Received packet " << seq_num << "\n";
            stats.packets_received++;
            packet_queue.push(seq_num, data);

            if (seq_num == expected_seq_num) {
                send_ack(sock, seq_num, client_addr);
                expected_seq_num++;
            } else {
                stats.out_of_order++;
                cout << "[Receiver] Out of order packet. Expected " 
                     << expected_seq_num << ", got " << seq_num << "\n";
                send_ack(sock, expected_seq_num - 1, client_addr);
            }
        } else {
            stats.corrupted_packets++;
            cerr << "[Receiver] Invalid packet received\n";
        }
    }

    running = false;
    processor.join();
    cout << "[Receiver] Terminating due to " << MAX_TIMEOUTS << " consecutive timeouts\n";
    close(sock);
    stats.print();
}

void go_back_n_receiver() {
    int sock = create_receiver_socket();
    
    ReceiverStats stats;
    int expected_seq_num = 0;
    vector<bool> received_packets(1000, false);

    cout << "[Receiver] Started in Go-Back-N mode. Waiting for packets...\n";

    while (true) {
        char buffer[MAX_BUFFER_SIZE] = {0};
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        
        int bytes_received = recvfrom(sock, buffer, sizeof(buffer), 0, 
                                    (sockaddr*)&client_addr, &addr_len);

        if (bytes_received > 0) {
            string packet(buffer, bytes_received);
            int seq_num;
            string data;
            if (validate_packet(packet, seq_num, data)) {
                cout << "[Receiver] Received packet " << seq_num << "\n";
                stats.packets_received++;
                stats.total_bytes_received += data.length();
                process_received_data(data);

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
            } else {
                stats.corrupted_packets++;
                cerr << "[Receiver] Invalid packet received\n";
            }
        }
    }

    close(sock);
    stats.print();
}

void selective_repeat_receiver() {
    int sock = create_receiver_socket();
    
    ReceiverStats stats;
    int expected_seq_num = 0;
    vector<bool> received_packets(1000, false);
    vector<string> packet_buffer(1000);

    cout << "[Receiver] Started in Selective Repeat mode. Waiting for packets...\n";

    while (true) {
        char buffer[MAX_BUFFER_SIZE] = {0};
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        
        int bytes_received = recvfrom(sock, buffer, sizeof(buffer), 0, 
                                    (sockaddr*)&client_addr, &addr_len);

        if (bytes_received > 0) {
            string packet(buffer, bytes_received);
            int seq_num;
            string data;
            if (validate_packet(packet, seq_num, data)) {
                cout << "[Receiver] Received packet " << seq_num << "\n";
                stats.packets_received++;
                stats.total_bytes_received += data.length();
                process_received_data(data);

                if (seq_num >= expected_seq_num) {
                    received_packets[seq_num] = true;
                    packet_buffer[seq_num] = string(buffer, bytes_received);
                    send_ack(sock, seq_num, client_addr);
                    
                    while (received_packets[expected_seq_num]) {
                        cout << "[Receiver] Delivering packet " << expected_seq_num << "\n";
                        expected_seq_num++;
                    }
                } else {
                    stats.out_of_order++;
                    cout << "[Receiver] Out of order packet " << seq_num << "\n";
                    send_ack(sock, seq_num, client_addr);
                }
            } else {
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
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    print_available_interfaces();
    cout << "Receiver started. Waiting for packets...\n";

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