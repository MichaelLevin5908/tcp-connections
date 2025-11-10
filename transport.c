#include "consts.h"
#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/*
 * the following variables are only informational,
 * not necessarily required for a valid implementation.
 * feel free to edit/remove.
 */
int state = 0;                             // Current state for handshake
int our_send_window = 0;                   // Total number of bytes in our send buf
int their_receiving_window = MIN_WINDOW;   // Receiver window size (starts at MIN_WINDOW)
int our_max_receiving_window = MAX_WINDOW; // Our max receiving window
int our_recv_window = 0;                   // Bytes in our recv buf
int dup_acks = 0;                          // Duplicate acknowledgements received
uint32_t ack = 0;                          // Acknowledgement number
uint32_t seq = 0;                          // Sequence number
uint32_t last_ack = 0;                     // Last ACK number to keep track of duplicate ACKs
bool pure_ack = false;                     // Require ACK to be sent out
packet *base_pkt = NULL;                   // Lowest outstanding packet to be sent out
uint32_t next_seq = 0;                     // Next sequence number we expect to receive

buffer_node *recv_buf =
    NULL; // Linked list storing out of order received packets
buffer_node *send_buf =
    NULL; // Linked list storing packets that were sent but not acknowledged

ssize_t (*input)(uint8_t *, size_t); // Get data from layer
void (*output)(uint8_t *, size_t);   // Output data from layer

struct timeval start; // Last packet sent at this time
struct timeval now;   // Temp for current time

// Get data from standard input / make handshake packets
packet *get_data()
{
    switch (state)
    {
    // Client states:
    case CLIENT_START:
    {
        // Client: sends first SYN , attempting to build a connection
        packet *syn_packet = malloc(sizeof(packet) + MAX_PAYLOAD);
        if (!syn_packet)
            return NULL;
        memset(syn_packet, 0, sizeof(packet) + MAX_PAYLOAD);

        syn_packet->seq = htons(seq);
        syn_packet->flags = SYN;
        syn_packet->win = htons(our_max_receiving_window);
        syn_packet->length = htons(0);

        state = CLIENT_AWAIT;
        return syn_packet;
    }
    case CLIENT_AWAIT:
        return NULL;

    // Server states:
    case SERVER_START:
    {
        // Server: sends SYN|ACK after receiving SYN from the client
        packet *synack_packet = malloc(sizeof(packet) + MAX_PAYLOAD);
        if (!synack_packet)
            return NULL;
        memset(synack_packet, 0, sizeof(packet) + MAX_PAYLOAD);

        synack_packet->seq = htons(seq);
        synack_packet->ack = htons(ack);
        synack_packet->flags = SYN | ACK;
        synack_packet->length = htons(0);
        synack_packet->win = htons(our_max_receiving_window);

        seq += 1;
        state = SERVER_AWAIT;
        return synack_packet;
    }
    case SERVER_AWAIT:
        return NULL;

    case NORMAL:
    {
        if (pure_ack)
        {
            packet *ack_pkt = calloc(1, sizeof(packet) + MAX_PAYLOAD);
            if (!ack_pkt)
                return NULL;

            ack_pkt->seq = htons(seq);
            ack_pkt->ack = htons(ack);
            ack_pkt->flags = ACK;
            ack_pkt->win = htons(our_max_receiving_window - our_recv_window);

            pure_ack = false;
            return ack_pkt;
        }

        size_t window_room =
            (their_receiving_window > our_send_window) ? (their_receiving_window - our_send_window) : 0;
        if (window_room == 0)
            return NULL;

        packet *data_pkt = calloc(1, sizeof(packet) + MAX_PAYLOAD);
        if (!data_pkt)
            return NULL;

        size_t chunk = MIN(MAX_PAYLOAD, window_room);
        ssize_t bytes_read = input(data_pkt->payload, chunk);
        if (bytes_read <= 0)
        {
            free(data_pkt);
            return NULL;
        }

        data_pkt->seq = htons(seq);
        data_pkt->ack = htons(ack);
        data_pkt->flags = ACK;
        data_pkt->length = htons(bytes_read);
        data_pkt->win = htons(our_max_receiving_window - our_recv_window);

        buffer_node **tail = &send_buf;
        while (*tail != NULL)
            tail = &(*tail)->next;

        buffer_node *node = malloc(sizeof(buffer_node) + bytes_read);
        if (!node)
        {
            free(data_pkt);
            return NULL;
        }
        memcpy(&node->pkt, data_pkt, sizeof(packet) + bytes_read);
        node->next = NULL;
        *tail = node;

        our_send_window += bytes_read;
        seq += 1;
        base_pkt = send_buf ? &send_buf->pkt : NULL;

        return data_pkt;
    }

    default:
    {
        return NULL;
    }
    }
}

// Process data received from socket
void recv_data(packet *pkt)
{
    switch (state)
    {
    // Server states:
    case SERVER_AWAIT:
    {
        if (pkt->flags & SYN && !(pkt->flags & ACK))
        {
            // initial SYN from client
            ack = ntohs(pkt->seq) + 1;
            next_seq = ack;
            state = SERVER_START;
        }
        else if (pkt->flags & ACK)
        {
            // final ACK received, handshake complete
            state = NORMAL;
        }
        break;
    }
    case SERVER_START: // No actions
        break;

    // Client states:
    case CLIENT_AWAIT:
    {
        // Client: receives SYN|ACK
        if ((pkt->flags & SYN) && (pkt->flags & ACK))
        {
            pure_ack = true;
            // store server's seq + 1 as our ACK number
            ack = ntohs(pkt->seq) + 1;
            next_seq = ack;
            // incr our own sequence number for the final ACK
            seq += 1;
            state = NORMAL;
        }
        break;
    }
    case CLIENT_START: // No actions
        break;
    case NORMAL:
    {
        uint16_t pkt_seq = ntohs(pkt->seq);
        uint16_t pkt_ack = ntohs(pkt->ack);
        uint16_t pkt_len = ntohs(pkt->length);

        their_receiving_window = ntohs(pkt->win);

        if (pkt->flags & ACK)
        {
            if (pkt_ack > last_ack)
            {
                buffer_node **link = &send_buf;
                while (*link != NULL)
                {
                    buffer_node *current = *link;
                    uint16_t buf_seq = ntohs(current->pkt.seq);
                    uint16_t buf_len = ntohs(current->pkt.length);

                    if (buf_seq < pkt_ack)
                    {
                        our_send_window -= buf_len;
                        *link = current->next;
                        free(current);
                    }
                    else
                    {
                        link = &current->next;
                    }
                }

                base_pkt = send_buf ? &send_buf->pkt : NULL;
                last_ack = pkt_ack;
                dup_acks = 0;
            }
            else if (pkt_ack == last_ack && last_ack != 0)
            {
                dup_acks += 1;
            }
        }

        if (pkt_len > 0)
        {
            if (pkt_seq == ack)
            {
                output(pkt->payload, pkt_len);
                ack += 1;

                bool progressed;
                do
                {
                    progressed = false;
                    buffer_node **link = &recv_buf;
                    while (*link != NULL)
                    {
                        buffer_node *current = *link;
                        uint16_t buf_seq = ntohs(current->pkt.seq);
                        uint16_t buf_len = ntohs(current->pkt.length);

                        if (buf_seq == ack)
                        {
                            output(current->pkt.payload, buf_len);
                            ack += 1;
                            our_recv_window -= buf_len;
                            *link = current->next;
                            free(current);
                            progressed = true;
                            break;
                        }
                        else
                        {
                            link = &current->next;
                        }
                    }
                } while (progressed);

                pure_ack = true;
            }
            else if (pkt_seq > ack)
            {
                buffer_node *node = malloc(sizeof(buffer_node) + pkt_len);
                memcpy(&node->pkt, pkt, sizeof(packet) + pkt_len);
                node->next = recv_buf;
                recv_buf = node;
                our_recv_window += pkt_len;
                pure_ack = true;
            }
            else
            {
                pure_ack = true;
            }
        }
        break;
    }

    default:
    {
        break;
    }
    }
}

// Main function of transport layer; never quits
void listen_loop(int sockfd, struct sockaddr_in *addr, int initial_state,
                 ssize_t (*input_p)(uint8_t *, size_t),
                 void (*output_p)(uint8_t *, size_t))
{

    // Set initial state (whether client or server)
    state = initial_state;

    // Set input and output function pointers
    input = input_p;
    output = output_p;

    // Set socket for nonblocking
    int flags = fcntl(sockfd, F_GETFL);
    flags |= O_NONBLOCK;
    fcntl(sockfd, F_SETFL, flags);
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &(int){1}, sizeof(int));

    // Set initial sequence number
    uint32_t r;
    int rfd = open("/dev/urandom", 'r');
    read(rfd, &r, sizeof(uint32_t));
    close(rfd);
    srand(r);
    seq = (rand() % 10) * 100 + 100;

    // Setting timers
    gettimeofday(&now, NULL);
    gettimeofday(&start, NULL);

    // Create buffer for incoming data
    char buffer[sizeof(packet) + MAX_PAYLOAD] = {0};
    packet *pkt = (packet *)&buffer;
    socklen_t addr_size = sizeof(struct sockaddr_in);

    // Start listen loop
    for (;;)
    {
        memset(buffer, 0, sizeof(packet) + MAX_PAYLOAD);

        ssize_t bytes_recvd =
            recvfrom(sockfd, &buffer, sizeof(buffer), 0, (struct sockaddr *)addr, &addr_size);
        if (bytes_recvd > 0)
        {
            print_diag(pkt, RECV);
            recv_data(pkt);
        }

        bool sent_data = false;
        for (packet *tosend = get_data(); tosend != NULL; tosend = get_data())
        {
            print_diag(tosend, SEND);
            sendto(sockfd, tosend, sizeof(packet) + ntohs(tosend->length), 0,
                   (struct sockaddr *)addr, sizeof(struct sockaddr_in));
            sent_data = true;
            free(tosend);
        }

        if (sent_data)
        {
            gettimeofday(&start, NULL);
        }

        if (pure_ack)
        {
            packet *ack_pkt = get_data();
            if (ack_pkt != NULL)
            {
                print_diag(ack_pkt, SEND);
                sendto(sockfd, ack_pkt, sizeof(packet) + ntohs(ack_pkt->length), 0,
                       (struct sockaddr *)addr, sizeof(struct sockaddr_in));
                free(ack_pkt);
            }
        }

        gettimeofday(&now, NULL);
        if (base_pkt != NULL && TV_DIFF(now, start) >= RTO)
        {
            print_diag(base_pkt, RTOD);
            sendto(sockfd, base_pkt, sizeof(packet) + ntohs(base_pkt->length), 0,
                   (struct sockaddr *)addr, sizeof(struct sockaddr_in));
            gettimeofday(&start, NULL);
        }
        else if (base_pkt != NULL && dup_acks >= DUP_ACKS)
        {
            print_diag(base_pkt, DUPA);
            sendto(sockfd, base_pkt, sizeof(packet) + ntohs(base_pkt->length), 0,
                   (struct sockaddr *)addr, sizeof(struct sockaddr_in));
            gettimeofday(&start, NULL);
            dup_acks = 0;
        }
        else if (base_pkt == NULL)
        {
            gettimeofday(&start, NULL);
        }
    }
}
