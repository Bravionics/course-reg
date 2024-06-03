#include "server.h"
#include "protocol.h"
#include <pthread.h>
#include <signal.h>

const char exit_str[] = "exit";

char buffer[BUFFER_SIZE];
pthread_mutex_t buffer_lock; 

int total_num_msg = 0;
int listen_fd;

//my definitions
pthread_mutex_t stats_mutex;

list_t * userList;
pthread_rwlock_t userList_rwlock;

FILE * logFile;
pthread_mutex_t logFile_mutex;

pthread_mutex_t courseArray_mutexes[32];
volatile sig_atomic_t shutdown_flag = 0;
//definitions end

void sigint_handler(int sig)
{
    shutdown_flag = 1;

    //send sigint to threads
    pthread_rwlock_rdlock(&userList_rwlock);
    node_t * curr = userList->head;
    while (curr != NULL) {
        user_t * temp = (user_t *)curr->data;
        if (temp->tid != 0) {
            pthread_kill(temp->tid, SIGINT);
        }
        curr = curr->next;
    }
    pthread_rwlock_unlock(&userList_rwlock);

    //pthread join the threads
    curr = userList->head;
    while(curr != NULL) {
        user_t * temp = (user_t *)curr->data;
        if (temp->tid != 0) {
            pthread_join(temp->tid, NULL);
        }
        curr = curr->next;
    }

    // Output the current state of all courses to STDOUT
    
    for (int i = 0; i < 32; ++i) {
        pthread_mutex_lock(&courseArray_mutexes[i]);
        if (courseArray[i].title != NULL) {
            printf("%s, %d, %d, ", courseArray[i].title, courseArray[i].maxCap, courseArray[i].enrollment->length);
            
            // Output enrolled usernames in alphabetical order
            node_t* node = courseArray[i].enrollment->head;
            while (node != NULL) {
                printf("%s", (char *)node->data);
                if (node->next != NULL) {
                    printf(";");
                }
                node = node->next;
            }
            
            printf(", ");
            
            // Output waitlist usernames in waitlist order
            node = courseArray[i].waitlist->head;
            while (node != NULL) {
                printf("%s", (char*)node->data);
                if (node->next != NULL) {
                    printf(";");
                }
                node = node->next;
            }
            
            printf("\n");
        }
        pthread_mutex_unlock(&courseArray_mutexes[i]);
    }

    //output users to stderr
    curr = userList->head;
    while (curr != NULL) {
        user_t* user = (user_t*)curr->data;
        fprintf(stderr, "%s, %u, %u\n", user->username, user->enrolled, user->waitlisted);
        curr = curr->next;
    }

    //curStats to stderr
    fprintf(stderr, "%d, %d, %d, %d\n", curStats.clientCnt, curStats.threadCnt, curStats.totalAdds, curStats.totalDrops);
}

int user_comparator(const void * a, const void * b) {
    const user_t * A = (const user_t *) a;
    const user_t * B = (const user_t *) b;
    return strcmp(A->username, B->username);
}

int read_courses (const char * file_name) {
    FILE * f = fopen(file_name, "r");
    if (!f) {
        printf("ERROR: Could not open course file\n");
        exit(2);
    }

    char line[256];
    int index = 0;
    while (fgets(line, sizeof(line), f)) {
        char * title = strtok(line, ";");
        char * temp = strtok(NULL, ";");
        int max = atoi(temp);

        courseArray[index].title = strdup(title);
        courseArray[index].maxCap = max;
        courseArray[index].enrollment = CreateList(user_comparator, NULL, NULL);
        courseArray[index].waitlist = CreateList(user_comparator, NULL, NULL);
        index++;
    }
    fclose(f);

    return index;
}

int server_init(int server_port){
    int sockfd;
    struct sockaddr_in servaddr;

    // socket create and verification
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        printf("socket creation failed...\n");
        exit(1);
    }
    else
        //printf("Socket successfully created\n");

    bzero(&servaddr, sizeof(servaddr));

    // assign IP, PORT
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(server_port);

    int opt = 1;
    if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, (char *)&opt, sizeof(opt))<0)
    {
	perror("setsockopt");exit(EXIT_FAILURE);
    }

    // Binding newly created socket to given IP and verification
    if ((bind(sockfd, (SA*)&servaddr, sizeof(servaddr))) != 0) {
        printf("socket bind failed\n");
        exit(EXIT_FAILURE);
    }
    else
        //printf("Socket successfully binded\n");

    // Now server is ready to listen and verification
    if ((listen(sockfd, 1)) != 0) {
        printf("Listen failed\n");
        exit(EXIT_FAILURE);
    }
    else
        //printf("Currently listening on port %d.\n", server_port);

    return sockfd;
}

//Function running in thread
void *process_client(void* user_struct_ptr){
    user_t thread_user = *(user_t *)user_struct_ptr;

    while(!shutdown_flag){
        petrV_header header;
        if (rd_msgheader(thread_user.socket_fd, &header) != 0) {
            close(thread_user.socket_fd);
            return NULL;
        }
        //read body of the message
        char * body = malloc(header.msg_len);
        if (read(thread_user.socket_fd, body, header.msg_len) < 0) {
            free(body);
            close(thread_user.socket_fd);
            return NULL;
        }

        switch (header.msg_type) {
        case LOGOUT:
        {
            int temp_socket_fd = thread_user.socket_fd;
            pthread_rwlock_wrlock(&userList_rwlock);
            thread_user.socket_fd = 0;
            pthread_rwlock_unlock(&userList_rwlock);

            header.msg_type = OK;
            header.msg_len = 0;
            wr_msg(temp_socket_fd, &header, "");

            pthread_mutex_lock(&logFile_mutex);
            fprintf(logFile, "%s LOGOUT\n", thread_user.username);
            pthread_mutex_unlock(&logFile_mutex);

            close(temp_socket_fd);

            free(body);
            fflush(logFile);
            return NULL;
        }
        case CLIST: //list courses on the server
        {
            char response_text[BUFFER_SIZE] = {0};
            for (int i = 0; i < 32; ++i) {
                pthread_mutex_lock(&courseArray_mutexes[i]);
                if (courseArray[i].title != NULL) {
                    
                    char course_info[128];
                    if (courseArray[i].enrollment->length >= courseArray[i].maxCap) {
                        snprintf(course_info, sizeof(course_info), "Course %d - %s (CLOSED)\n", i, courseArray[i].title);
                    } else {
                        snprintf(course_info, sizeof(course_info), "Course %d - %s\n", i, courseArray[i].title);
                    }
                    strncat(response_text, course_info, sizeof(response_text) - strlen(response_text) - 1);
                    
                }
                pthread_mutex_unlock(&courseArray_mutexes[i]);
            }
            header.msg_len = strlen(response_text);
            header.msg_type = CLIST;
            wr_msg(thread_user.socket_fd, &header, response_text);

            pthread_mutex_lock(&logFile_mutex);
            fprintf(logFile, "%s CLIST\n", thread_user.username);
            pthread_mutex_unlock(&logFile_mutex);

            fflush(logFile);
            break;
        }
        case SCHED:
        {
            char response_txt[BUFFER_SIZE] = {0};
            int en_or_wait = 0;

            user_t * matched_user = NULL;
            pthread_rwlock_rdlock(&userList_rwlock);
            node_t * curr_node = userList->head;
            while (curr_node != NULL) {
                user_t * temp_user = (user_t*)curr_node->data;
                if (strcmp(temp_user->username, thread_user.username) == 0) {
                    matched_user = temp_user;
                    break;
                }
                curr_node = curr_node->next;
            }
            pthread_rwlock_unlock(&userList_rwlock);

            for (int i = 0; i < 32; ++i) {
                int isEnrolled = matched_user->enrolled & (1 << i);
                int isWaitlisted = matched_user->waitlisted & (1 << i);

                if (isEnrolled || isWaitlisted) {
                    en_or_wait = 1;
                    snprintf(response_txt + strlen(response_txt), sizeof(response_txt) - strlen(response_txt), "Course %d - %s%s\n", i, courseArray[i].title, isWaitlisted ? " (WAITING)" : "");
                }
            }

            if (!en_or_wait) {
                header.msg_len = 0;
                header.msg_type = ENOCOURSES;
                wr_msg(thread_user.socket_fd, &header, "");

                pthread_mutex_lock(&logFile_mutex);
                fprintf(logFile, "%s NOSCHED\n", thread_user.username);
                pthread_mutex_unlock(&logFile_mutex);
            } else {
                header.msg_len = strlen(response_txt);
                header.msg_type = SCHED;
                wr_msg(thread_user.socket_fd, &header, response_txt);

                pthread_mutex_lock(&logFile_mutex);
                fprintf(logFile, "%s SCHED\n", thread_user.username);
                pthread_mutex_unlock(&logFile_mutex);
            }

            fflush(logFile);
            break;
        }
            
        case ENROLL:
        {
            int index = atoi(body);
            pthread_mutex_lock(&courseArray_mutexes[index]);

            if (index >= 32 || courseArray[index].title == NULL) {
                header.msg_type = ECNOTFOUND;
                header.msg_len = 0;
                wr_msg(thread_user.socket_fd, &header, "");
                fprintf(logFile, "%s NOTFOUND_E %d\n", thread_user.username, index);

            } else if (thread_user.enrolled & (1 << index) || courseArray[index].enrollment->length >= courseArray[index].maxCap) {
                header.msg_type = ECDENIED;
                header.msg_len = 0;
                wr_msg(thread_user.socket_fd, &header, "");
                fprintf(logFile, "%s NOENROLL %d\n", thread_user.username, index);
            } else {
                //insert into class list and mark as enrolled in user_t
                InsertAtTail(courseArray[index].enrollment, thread_user.username);
                thread_user.enrolled |= (1 << index);

                //fix to make sure bitvector is set
                pthread_rwlock_wrlock(&userList_rwlock);
                node_t * curr = userList->head;
                while (curr != NULL) {
                    user_t * temp_user = (user_t *)curr->data;
                    if (strcmp(temp_user->username, thread_user.username) == 0) {
                        temp_user->enrolled |= (1 << index);
                        break;
                    }
                    curr = curr->next;
                }
                pthread_rwlock_unlock(&userList_rwlock);

                //return response
                header.msg_type = OK;
                header.msg_len = 0;
                wr_msg(thread_user.socket_fd, &header, "");

                //write to log
                pthread_mutex_lock(&logFile_mutex);
                fprintf(logFile, "%s ENROLL %d %d\n", thread_user.username, index, thread_user.enrolled);
                pthread_mutex_unlock(&logFile_mutex);
            }
            pthread_mutex_unlock(&courseArray_mutexes[index]);

            //curStats upddate
            pthread_mutex_lock(&stats_mutex);
            curStats.totalAdds++;
            pthread_mutex_unlock(&stats_mutex);

            fflush(logFile);
            break;
        }
        case WAIT:
        {
            int index = atoi(body);

            if (index >= 32 || index < 0) {
                header.msg_type = ECNOTFOUND;
                header.msg_len = 0;
                wr_msg(thread_user.socket_fd, &header, "");

                pthread_mutex_lock(&logFile_mutex);
                fprintf(logFile, "%s NOTFOUND_W %d\n", thread_user.username, index);
                pthread_mutex_unlock(&logFile_mutex);
                break;
            }

            pthread_mutex_lock(&courseArray_mutexes[index]);

            if (courseArray[index].title == NULL) {
                header.msg_type = ECNOTFOUND;
                header.msg_len = 0;
                wr_msg(thread_user.socket_fd, &header, "");

                pthread_mutex_lock(&logFile_mutex);
                fprintf(logFile, "%s NOTFOUND_W %d\n", thread_user.username, index);
                pthread_mutex_unlock(&logFile_mutex);
            } else if (courseArray[index].enrollment->length < courseArray[index].maxCap || (thread_user.enrolled & (1 << index))) {
                header.msg_type = ECDENIED;
                header.msg_len = 0;
                wr_msg(thread_user.socket_fd, &header, "");

                pthread_mutex_lock(&logFile_mutex);
                fprintf(logFile, "%s NOWAIT %d\n", thread_user.username, index);
                pthread_mutex_unlock(&logFile_mutex);
            } else {
                //insert into waitlist and mark as waitlisted in user_t
                InsertAtTail(courseArray[index].waitlist, thread_user.username);
                thread_user.waitlisted |= (1 << index);

                //fix to make sure bitvector is set
                pthread_rwlock_wrlock(&userList_rwlock);
                node_t * curr = userList->head;
                while (curr != NULL) {
                    user_t * temp_user = (user_t *)curr->data;
                    if (strcmp(temp_user->username, thread_user.username) == 0) {
                        temp_user->waitlisted |= (1 << index);
                        break;
                    }
                    curr = curr->next;
                }
                pthread_rwlock_unlock(&userList_rwlock);
                

                //return response
                header.msg_type = OK;
                header.msg_len = 0;
                wr_msg(thread_user.socket_fd, &header, "");

                //write to log
                pthread_mutex_lock(&logFile_mutex);
                fprintf(logFile, "%s WAIT %d %d\n", thread_user.username, index, thread_user.waitlisted);
                pthread_mutex_unlock(&logFile_mutex);
            }
            pthread_mutex_unlock(&courseArray_mutexes[index]);
            fflush(logFile);
            break;
        }
        
        case DROP:
        {
            int index = atoi(body);
            pthread_mutex_lock(&courseArray_mutexes[index]);

            if (index >= 32 || strlen(courseArray[index].title) <= 0) {
                header.msg_type = ECNOTFOUND;
                header.msg_len = 0;
                wr_msg(thread_user.socket_fd, &header, "");

                pthread_mutex_lock(&logFile_mutex);
                fprintf(logFile, "%s NOTFOUND_D %d\n", thread_user.username, index);
                pthread_mutex_unlock(&logFile_mutex);
            } else if (!(thread_user.enrolled & (1 << index))) {
                header.msg_type = ECDENIED;
                header.msg_len = 0;
                wr_msg(thread_user.socket_fd, &header, "");

                pthread_mutex_lock(&logFile_mutex);
                fprintf(logFile, "%s NODROP %d\n", thread_user.username, index);
                pthread_mutex_unlock(&logFile_mutex);
            } else {
                //search for and remove username from course list
                int rem_index = -1;
                int counter = 0;
                node_t* curr = courseArray[index].enrollment->head;
                while (curr != NULL) {
                    if (strcmp(((char *)curr->data), thread_user.username) == 0) {
                        rem_index = counter;
                        break;
                    }
                    curr = curr->next;
                    counter++;
                }
                RemoveByIndex(courseArray[index].enrollment, rem_index);
                thread_user.enrolled &= ~(1 << index);

                //fix to make sure bitvector is set
                pthread_rwlock_wrlock(&userList_rwlock);
                curr = userList->head;
                while (curr != NULL) {
                    user_t * temp_user = (user_t *)curr->data;
                    if (strcmp(temp_user->username, thread_user.username) == 0) {
                        temp_user->enrolled &= ~(1 << index);
                        break;
                    }
                    curr = curr->next;
                }
                pthread_rwlock_unlock(&userList_rwlock);


                //stats
                pthread_mutex_lock(&stats_mutex);
                curStats.totalDrops++;
                pthread_mutex_unlock(&stats_mutex);

                //respond to client
                header.msg_type = OK;
                header.msg_len = 0;
                wr_msg(thread_user.socket_fd, &header, "");

                //log
                pthread_mutex_lock(&logFile_mutex);
                fprintf(logFile, "%s DROP %d %d\n", thread_user.username, index, thread_user.enrolled);
                pthread_mutex_unlock(&logFile_mutex);

                //waitlist post drop logic
                if (courseArray[index].waitlist->length > 0) {
                    char * add_from_wait_username = RemoveFromHead(courseArray[index].waitlist);
                    pthread_rwlock_rdlock(&userList_rwlock);
                    node_t* curr_user_node = userList->head;
                    user_t* nextUser = NULL;
                    while (curr_user_node != NULL) {
                        user_t* temp_user = (user_t*)curr_user_node->data;
                        if (strcmp(temp_user->username, add_from_wait_username) == 0) {
                            nextUser = temp_user;
                            break;
                        }
                        curr_user_node = curr_user_node->next;
                    }
                    pthread_rwlock_unlock(&userList_rwlock);
                    InsertAtTail(courseArray[index].enrollment, add_from_wait_username);
                    nextUser->enrolled |= (1 << index);
                    nextUser->waitlisted &= ~(1 << index);

                    //fix to make sure bitvector is set
                    pthread_rwlock_wrlock(&userList_rwlock);
                    curr = userList->head;
                    while (curr != NULL) {
                        user_t * temp_user = (user_t *)curr->data;
                        if (strcmp(temp_user->username, nextUser->username) == 0) {
                            temp_user->enrolled |= (1 << index);
                            temp_user->waitlisted &= ~(1 << index);
                            break;
                        }
                        curr = curr->next;
                    }
                    pthread_rwlock_unlock(&userList_rwlock);

                    pthread_mutex_lock(&stats_mutex);
                    curStats.totalAdds++;
                    pthread_mutex_unlock(&stats_mutex);

                    pthread_mutex_lock(&logFile_mutex);
                    fprintf(logFile, "%s WAITADD %d %d\n", add_from_wait_username, index, nextUser->enrolled);
                    pthread_mutex_unlock(&logFile_mutex);
                }
            }

            pthread_mutex_unlock(&courseArray_mutexes[index]);
            fflush(logFile);
            break;
        }
            
        default:
            break;
        }
    }
    // Close the socket at the end
    printf("Close current client connection\n");
    close(thread_user.socket_fd);

    free(user_struct_ptr);
    return NULL;
}

void run_server(int server_port, char * course_filename, char * log_filename){
    listen_fd = server_init(server_port); // Initiate server and start listening on specified port

    // Initialize stats
    curStats.clientCnt = 0;
    curStats.threadCnt = 0;
    curStats.totalAdds = 0;
    curStats.totalDrops = 0;
    pthread_mutex_init(&stats_mutex, NULL);

    // Initialize user_t linked list
    userList = CreateList(user_comparator, NULL, NULL); //compare, print, delete
    pthread_rwlock_init(&userList_rwlock, NULL);

    // Read in course to course array
    int course_amt = read_courses(course_filename);
    printf("Server initialized with %d courses.\n", course_amt);

    // Open log file for writing
    logFile = fopen(log_filename, "w");
    if (!logFile) {
        printf("ERROR: Could not open log file\n");
        exit(2);
    }
    pthread_mutex_init(&logFile_mutex, NULL);

    // Initialize courseArray mutexes
    for (int i = 0; i < 32; ++i) {
        pthread_mutex_init(&courseArray_mutexes[i], NULL);
    }

    //initialization complete
    printf("Currently listening on port %d.\n", server_port);

    int client_fd;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    //install signal handler
    struct sigaction myaction = {{0}};
    myaction.sa_handler = sigint_handler;
    if (sigaction(SIGINT, &myaction, NULL) == -1){
        printf("signal handler failed to install\n");
    }
    
    while(!shutdown_flag){
        // Wait and Accept the connection from client
        //printf("Wait for new client connection\n");
        int* client_fd = malloc(sizeof(int));
        *client_fd = accept(listen_fd, (SA*)&client_addr, &client_addr_len);
        if (*client_fd < 0) {
            //printf("server acccept failed\n");
            exit(EXIT_FAILURE);
        }
        else{
            printf("Client connection accepted\n");
            //process header
            petrV_header header;
            if (rd_msgheader(*client_fd, &header) != 0) {
                close(*client_fd);
                return;
            }
            //Check to make sure it is login
            if (header.msg_type != LOGIN) {
                close(*client_fd);
                return;
            }
            //read body of the message
            char * username = malloc(header.msg_len);
            if (read(*client_fd, username, header.msg_len) < 0) {
                free(username);
                close(*client_fd);
                return;
            }
            //check username against userList and respond appropriately
            pthread_rwlock_wrlock(&userList_rwlock);
            user_t * user = NULL;
            node_t * curr = userList->head;
            while (curr != NULL) {
                user_t * temp = (user_t *)curr->data;
                if (strcmp(temp->username, username) == 0) {
                    user = temp;
                    break;
                }
                curr = curr->next;
            }


            //handle found or not found user
            if (user == NULL) {
                user = malloc(sizeof(user_t));
                user->username = strdup(username);
                user->socket_fd = *client_fd;
                user->tid = 0;
                user->enrolled = 0;
                user->waitlisted = 0;
                InsertInOrder(userList, user);

                pthread_mutex_lock(&logFile_mutex);
                fprintf(logFile, "CONNECTED %s\n", user->username);
                pthread_mutex_unlock(&logFile_mutex);
            } else{
                user->socket_fd = *client_fd;

                pthread_mutex_lock(&logFile_mutex);
                fprintf(logFile, "RECONNECTED %s\n", user->username);
                pthread_mutex_unlock(&logFile_mutex);
            }
            pthread_rwlock_unlock(&userList_rwlock);

            pthread_t tid;
            if (pthread_create(&tid, NULL, process_client, (void *)user) != 0) {
                free(username);
                close(*client_fd);
                free(username);
                return;
            }
            user->tid = tid;
            
            //header response to client
            header.msg_len = 0;
            header.msg_type = OK;
            wr_msg(*client_fd, &header, "OK");

            //stats
            pthread_mutex_lock(&stats_mutex);
            curStats.clientCnt++;
            curStats.threadCnt++;
            pthread_mutex_unlock(&stats_mutex);

            fflush(logFile);
            free(username);
            printf("Client thread created\n");
        }
    }
    bzero(buffer, BUFFER_SIZE);

    // Destroy the mutexes
    pthread_mutex_destroy(&stats_mutex); 
    pthread_rwlock_destroy(&userList_rwlock);
    pthread_mutex_destroy(&logFile_mutex);
    for (int i = 0; i < 32; ++i) {
        pthread_mutex_destroy(&courseArray_mutexes[i]);
    }

    fclose(logFile);

    close(listen_fd);
    return;
}



int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "h")) != -1) {
        switch (opt) {
            case 'h':
                fprintf(stderr, USAGE_MSG);
                exit(EXIT_FAILURE);
        }
    }

    // 3 positional arguments necessary
    if (argc != 4) {
        fprintf(stderr, USAGE_MSG);
        exit(EXIT_FAILURE);
    }
    unsigned int port_number = atoi(argv[1]);
    char * course_filename = argv[2];
    char * log_filename = argv[3];

    //INSERT CODE HERE
    run_server(port_number, course_filename, log_filename);

    return 0;
}
