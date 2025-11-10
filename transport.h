#pragma once

#include <stdint.h>
#include <unistd.h>

packet *get_data();
void recv_data(packet *pkt);

// Main function of transport layer; never quits
void listen_loop(int sockfd, struct sockaddr_in *addr, int type,
                 ssize_t (*input_p)(uint8_t *, size_t),
                 void (*output_p)(uint8_t *, size_t));

void add_to_buf(packet *p);
