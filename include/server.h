#ifndef SERVER_H
#define SERVER_H

#include <arpa/inet.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "linkedlist.h"

#define BUFFER_SIZE 1024
#define SA struct sockaddr

#define USAGE_MSG "./bin/zotReg_server [-h] PORT_NUMBER COURSE_FILENAME LOG_FILENAME"\
                  "\n  -h                 Displays this help menu and returns EXIT_SUCCESS."\
                  "\n  PORT_NUMBER        Port number to listen on."\
                  "\n  COURSE_FILENAME    File to read course information from at the start of the server"\
                  "\n  LOG_FILENAME       File to output server actions into. Create/overwrite, if exists\n"


typedef struct {
    int clientCnt;  
    int threadCnt;  
    int totalAdds;  
    int totalDrops; 
} stats_t;   

stats_t curStats;  

typedef struct {
    char* username;	
    int socket_fd;	
    pthread_t tid;
    uint32_t enrolled;	
    uint32_t waitlisted;
} user_t;

typedef struct {
    char* title; 
    int   maxCap;      
    list_t * enrollment; 
    list_t * waitlist;   
} course_t; 

course_t courseArray[32]; 

// INSERT FUNCTIONS HERE
int user_comparator(const void * a, const void * b);
int read_courses(const char * file_name);


#endif
