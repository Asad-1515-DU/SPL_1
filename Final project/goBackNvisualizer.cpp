#include <SFML/Graphics.hpp>
#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <sstream>

int PACKET_COUNT;
int WINDOW_SIZE;
const int PACKET_WIDTH = 40;
const int PACKET_HEIGHT = 40;
const int PADDING = 10;
const int START_X = 100;
const int SENDER_Y = 100;
const int RECEIVER_Y = 300;
const float PACKET_SPEED = 200.0f; // pixels per second
const float ACK_SPEED = 200.0f;     // pixels per second

enum PacketState { Idle, Sending, Sent, Receiving, Received, AckSending, Acked, Retransmitting };

struct Packet {
    sf::RectangleShape shape;
    PacketState state = Idle;
    float progress = 0.0f;
    bool inTransit = false;
    bool isRetransmission = false;  // Flag to indicate if it's a retransmission
    bool isLost = false;  // Flag to indicate if the packet is lost
    int sendCount = 0;  // Count of how many times the packet has been sent
};

void updatePacketColor(Packet& packet) {
    switch (packet.state) {
        case Idle:
            packet.shape.setFillColor(sf::Color(192, 192, 192)); // Gray
            break;
        case Sending:
            packet.shape.setFillColor(sf::Color::Green); // Sending to receiver
            break;
        case Sent:
            packet.shape.setFillColor(sf::Color::Blue); // Sent
            break;
        case Receiving:
            packet.shape.setFillColor(sf::Color::Magenta); // At receiver (changed to Magenta)
            break;
        case Received:
            packet.shape.setFillColor(sf::Color::Red); // Received at receiver
            break;
        case AckSending:
            packet.shape.setFillColor(sf::Color::Cyan); // ACK in transit
            break;
        case Acked:
            packet.shape.setFillColor(sf::Color::Yellow); // ACKed at sender
            break;
        case Retransmitting:
            packet.shape.setFillColor(sf::Color::Magenta); // Retransmitted packets in Magenta
            break;
    }
}

void readStatistics(int& packetCount, int& windowSize, std::vector<bool>& ackReceived, std::vector<int>& lostPackets) {
    std::ifstream stat_file("stat.txt");
    if (!stat_file.is_open()) {
        std::cerr << "Failed to open stat.txt for reading!" << std::endl;
        return;
    }

    std::string line;
    while (std::getline(stat_file, line)) {
        std::istringstream iss(line);
        std::string key;
        if (line.find("Total Packets:") != std::string::npos) {
            iss >> key >> key >> packetCount;
        } else if (line.find("Window Size:") != std::string::npos) {
            iss >> key >> key >> windowSize;
        } else if (line.find("ACK Received:") != std::string::npos) {
            ackReceived.clear();
            std::string ack;
            while (iss >> ack) {
                ackReceived.push_back(ack == "1");
            }
        } else if (line.find("Lost Packets:") != std::string::npos) {
            lostPackets.clear();
            int lost;
            while (iss >> lost) {
                lostPackets.push_back(lost);
            }
        }
    }
    stat_file.close();
}

int main() {
    std::vector<bool> ackReceived;
    std::vector<int> lostPackets;
    readStatistics(PACKET_COUNT, WINDOW_SIZE, ackReceived, lostPackets);

    sf::RenderWindow window(sf::VideoMode(1200, 500), "Go-Back-N ARQ Visualizer");

    sf::Font font;
    if (!font.loadFromFile("arial.ttf")) {
        std::cerr << "Failed to load font!" << std::endl;
        return 1;
    }

    std::vector<Packet> packets(PACKET_COUNT);
    for (int i = 0; i < PACKET_COUNT; ++i) {
        packets[i].shape.setSize(sf::Vector2f(PACKET_WIDTH, PACKET_HEIGHT));
        packets[i].shape.setPosition(START_X + i * (PACKET_WIDTH + PADDING), SENDER_Y);
        packets[i].state = ackReceived[i] ? Acked : Idle;
        packets[i].inTransit = false;
        packets[i].isRetransmission = false;
        packets[i].isLost = std::find(lostPackets.begin(), lostPackets.end(), i + 1) != lostPackets.end();
        packets[i].sendCount = 0;
        updatePacketColor(packets[i]);
    }

    int base = 0;
    int nextSeqNum = 0;
    bool running = false;
    bool retransmitting = false;

    sf::Clock clock;
    float timer = 0.0f;
    float sendInterval = 1.0f;

    while (window.isOpen()) {
        sf::Event event;
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed)
                window.close();

            if (event.type == sf::Event::KeyPressed) {
                if (event.key.code == sf::Keyboard::Space)
                    running = true;
                if (event.key.code == sf::Keyboard::R) {
                    for (int i = 0; i < PACKET_COUNT; ++i) {
                        packets[i].state = Idle;
                        packets[i].shape.setPosition(START_X + i * (PACKET_WIDTH + PADDING), SENDER_Y);
                        packets[i].progress = 0.0f;
                        packets[i].inTransit = false;
                        packets[i].isRetransmission = false;
                        packets[i].isLost = std::find(lostPackets.begin(), lostPackets.end(), i + 1) != lostPackets.end();
                        packets[i].sendCount = 0;
                        updatePacketColor(packets[i]);
                    }
                    base = 0;
                    nextSeqNum = 0;
                    running = false;
                }
            }
        }

        float deltaTime = clock.restart().asSeconds();
        timer += deltaTime;

        // Handle initial packet sending
        if (running && timer >= sendInterval) {
            if (nextSeqNum < base + WINDOW_SIZE && nextSeqNum < PACKET_COUNT && !packets[nextSeqNum].inTransit) {
                packets[nextSeqNum].state = Sending;
                packets[nextSeqNum].inTransit = true;
                packets[nextSeqNum].progress = 0.0f;
                packets[nextSeqNum].isRetransmission = false;  // Reset retransmission flag
                packets[nextSeqNum].sendCount++;
                updatePacketColor(packets[nextSeqNum]);
                timer = 0.0f;
            }
        }

        // Handle retransmissions for lost packets
        for (int i = 0; i < lostPackets.size(); ++i) {
            int lostPacketIndex = lostPackets[i] - 1;  // Lost packets are 1-indexed
            if (lostPacketIndex >= base && lostPacketIndex < base + WINDOW_SIZE && !packets[lostPacketIndex].inTransit && packets[lostPacketIndex].sendCount < 2) {
                // Trigger retransmission for lost packets (no ACK received)
                packets[lostPacketIndex].state = Sending;
                packets[lostPacketIndex].inTransit = true;
                packets[lostPacketIndex].progress = 0.0f;
                packets[lostPacketIndex].isRetransmission = true;  // Mark as retransmission
                packets[lostPacketIndex].sendCount++;
                updatePacketColor(packets[lostPacketIndex]);
                retransmitting = true;  // Indicate that a retransmission is happening
            }
        }

        // Move packets and update states
        for (int i = 0; i < PACKET_COUNT; ++i) {
            if (packets[i].state == Sending || packets[i].state == Retransmitting) {  // Send or retransmit
                float moveAmount = PACKET_SPEED * deltaTime;
                packets[i].progress += moveAmount;
                packets[i].shape.move(0, moveAmount);

                if (packets[i].shape.getPosition().y >= RECEIVER_Y) {
                    packets[i].state = Receiving;
                    packets[i].shape.setPosition(START_X + i * (PACKET_WIDTH + PADDING), RECEIVER_Y);
                    updatePacketColor(packets[i]);
                    packets[i].progress = 0.0f;
                }
            } else if (packets[i].state == Receiving) {
                packets[i].state = Received;
                updatePacketColor(packets[i]);
                if (!packets[i].isLost || packets[i].sendCount >= 2) {
                    packets[i].state = AckSending;
                    updatePacketColor(packets[i]);
                } else {
                    packets[i].inTransit = false;
                }
            } else if (packets[i].state == AckSending) {
                float moveAmount = ACK_SPEED * deltaTime;
                packets[i].progress += moveAmount;
                packets[i].shape.move(0, -moveAmount);

                if (packets[i].shape.getPosition().y <= SENDER_Y) {
                    packets[i].shape.setPosition(START_X + i * (PACKET_WIDTH + PADDING), SENDER_Y);
                    packets[i].state = Acked;
                    updatePacketColor(packets[i]);
                    packets[i].inTransit = false;

                    if (i == base) {
                        base++;
                        nextSeqNum = base;
                    }
                }
            }
        }

        window.clear(sf::Color::White);

        // Draw packets and retransmissions
        for (int i = 0; i < PACKET_COUNT; ++i) {
            if (i >= base && i < base + WINDOW_SIZE) {
                sf::RectangleShape windowBox;
                windowBox.setSize(sf::Vector2f(PACKET_WIDTH, PACKET_HEIGHT));
                windowBox.setOutlineThickness(4);
                windowBox.setOutlineColor(sf::Color::Black);
                windowBox.setFillColor(sf::Color::Transparent);
                windowBox.setPosition(START_X + i * (PACKET_WIDTH + PADDING), SENDER_Y);
                window.draw(windowBox);
            }
            window.draw(packets[i].shape);
        }

        for (int i = 0; i < PACKET_COUNT; ++i) {
            sf::RectangleShape receiverSlot;
            receiverSlot.setSize(sf::Vector2f(PACKET_WIDTH, PACKET_HEIGHT));
            receiverSlot.setOutlineThickness(1);
            receiverSlot.setOutlineColor(sf::Color::Black);
            receiverSlot.setFillColor(sf::Color::Transparent);
            receiverSlot.setPosition(START_X + i * (PACKET_WIDTH + PADDING), RECEIVER_Y);
            window.draw(receiverSlot);
        }

        sf::Text senderLabel("Sender", font, 20);
        senderLabel.setFillColor(sf::Color::Black);
        senderLabel.setPosition(10, SENDER_Y);
        window.draw(senderLabel);

        sf::Text receiverLabel("Receiver", font, 20);
        receiverLabel.setFillColor(sf::Color::Black);
        receiverLabel.setPosition(10, RECEIVER_Y);
        window.draw(receiverLabel);

        sf::Text instructions("SPACE: Start | R: Reset", font, 20);
        instructions.setFillColor(sf::Color::Black);
        instructions.setPosition(10, 450);
        window.draw(instructions);

        // Display base index on top
        sf::Text baseIndexLabel("Base Index: " + std::to_string(base), font, 20);
        baseIndexLabel.setFillColor(sf::Color::Black);
        baseIndexLabel.setPosition(10, 10);
        window.draw(baseIndexLabel);

        // Display available packets below base index
        std::string availablePackets;
        for (int i = base + 1; i < base + WINDOW_SIZE && i < PACKET_COUNT; ++i) {
            availablePackets += std::to_string(i) + " ";
        }
        sf::Text availablePacketsLabel("Available Packets: " + availablePackets, font, 20);
        availablePacketsLabel.setFillColor(sf::Color::Black);
        availablePacketsLabel.setPosition(10, 40);
        window.draw(availablePacketsLabel);

        window.display();
    }

    return 0;
}
