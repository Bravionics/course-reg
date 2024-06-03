#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

// These are the message types for the PetrV protocol
enum msg_types {
    OK,
    LOGIN,
    LOGOUT,
    CLIST,
    SCHED,
    ENROLL,
    DROP,
    WAIT,
    EUSRLGDIN = 0xF0,
    ECDENIED,
    ECNOTFOUND,
    ENOCOURSES,
    ESERV = 0xFF
};

// This is the struct describes the header of the PetrV protocol messages
typedef struct {
    uint32_t msg_len; // this should include the null terminator
    uint8_t msg_type;
} petrV_header;

int rd_msgheader(int socket_fd, petrV_header *h);
int wr_msg(int socket_fd, petrV_header *h, char *msgbuf);

#endif