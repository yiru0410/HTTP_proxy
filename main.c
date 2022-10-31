#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/queue.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>

int _proxy_socket;
int fline = 0;
char black_list[255] [255];
int NUM_THREAD = 8;
int thread_pool[8] = {0,0,0,0,0,0,0,0};
int thread_close[8] =  {0,0,0,0,0,0,0,0};
int tele_info;

#define MAXSIZE 65535

enum http_methods_enum {
    OPTIONS,
    GET,
    HEAD,
    POST,
    PUT,
    DELETE,
    TRACE,
    CONNECT,
    UNKNOWN,
    HTTP_METHODS_COUNT   
};

enum http_versions_enum {
    HTTP_VERSION_1_0,
    HTTP_VERSION_1_1,
    HTTP_VERSION_INVALID
};

typedef struct http_header
{
    enum http_methods_enum method;
    enum http_versions_enum version;
    const char *search_path;
    char *source;   
    char *host;                  
    TAILQ_HEAD(METADATA_HEAD, http_metadata_item) metadata_head;
} http_header_t;

typedef struct http_request{
    int client;
    int server;
    http_header_t *header;
    int8_t status;
    int thread_used;
    int isClient;
    int size;
    struct timeval timeStart, timeEnd;
    char* host;
}http_rquest_t;

http_rquest_t* thread_request[8];

typedef struct http_metadata_item
{
    const char *key;
    const char *value;
    TAILQ_ENTRY(http_metadata_item) entries;
} http_metadata_item_t;

int find_freeThread();
char *read_line(int socket);
void free_http_header(http_header_t *header);
http_header_t *read_header(int fd);
ssize_t send_data(int socket, const char* buf, size_t buf_size);
ssize_t pre_response(http_rquest_t* request);
void * do_exchange(void *arg);
void *exchange_data(void *arg);
int connect_server(http_header_t *header);
void implement_proxy(void *t);
void http_proxy(int port);

/**
 * @function: find_freeThread()
 * @usage: find the free thread
 * @return: the id of the free thread
 */
int find_freeThread(){
    int i;
    while(1){
        for(i=0; i<8; i++ ){
        if(thread_pool[i] == 0){
            return i;
            }
        }   
    }
    return -1;
}

/**
 * @function: read_line(int socket)
 * @usage: read_line from the socket.
 * @parameters: 
 * socket: the socket to be read
 */
char *read_line(int socket)
{
    int buf_size = 1;
    char *buffer = (char*)malloc(sizeof(char)*buf_size+1);
    char c;
    int ret = 0;
    int count = 0;
    
    while(1){
        ret = recv(socket, &c, 1, 0);
        if(ret > 0){
            buffer[count] = c;
            ++count;
        }
        if(c == '\n' || ret <= 0){
            buffer[count] = '\0';
            break;
        }
        if(buf_size == 1 || count == buf_size){
            buf_size += 128;
            buffer = (char*)realloc(buffer, sizeof(char)*buf_size + 1);
        }
    }
    return buffer;
}

/**
 * @function: free_http_header(http_header_t *header)
 * @usage: free the resource
 * @parameters: 
 * header: the header to be destroyed
 */
void free_http_header(http_header_t *header)
{
    struct http_metadata_item *item;
    
    TAILQ_FOREACH(item, &header->metadata_head, entries) {
        
        free((char*)item->key);
        free((char*)item->value);
        TAILQ_REMOVE(&header->metadata_head, item, entries);
        free(item);
    }
    free((char*)header->search_path);
    free((char*)header->source);
    free(header);
}

/**
 * @function: read_header(int fd)
 * @usage: parse the herder and store corresponding info
 * @parameters: 
 * @return: the header struct
 */
http_header_t *read_header(int fd){
    http_header_t *header = (http_header_t*)malloc(sizeof(http_header_t));
    if(header == NULL) return NULL;
    header->method = 0;
    header->search_path = NULL;
    header->source = NULL;
    TAILQ_INIT(&header->metadata_head);
    char *line;
    line = read_line(fd);
    if(!strlen(line)){
        // If there is no data received, then return null
        free_http_header(header);
        return NULL;
    }

    // parse the header
    char* line_full;
    char* line_split;
    char* split;
    int i = 0;
    line_full = line_split = strdup(line);
    char *http_methods[] ={"OPTIONS", "GET", "HEAD", "POST", "PUT", "DELETE", "TRACE", "CONNECT", "INVALID"};
    while ((split = strsep(&line_split, " \r\n")) != NULL && i<3){
        switch (i){
            case 0: {
                for (int j = 0; j <= HTTP_METHODS_COUNT; j++){
                    if (j == HTTP_METHODS_COUNT){
                        header->method = UNKNOWN;
                        free(line_full);
                    }
                    if (strcmp(split, http_methods[j]) == 0){
                        header->method = j;
                        break;
                    }
                }
            }
            case 1:{
                header->search_path = strdup(split);
                break;
            }
            case 2:{
                if(strcmp(split, "HTTP/1.0") == 0) {
                    header->version = HTTP_VERSION_1_0;
                } else if(strcmp(split, "HTTP/1.1") == 0) {
                    header->version = HTTP_VERSION_1_1;
                } else {
                    header->version = HTTP_VERSION_INVALID;
                }
                break;
            }
            default: printf("error\n");
        }
        i++;      
    }
    free(line_full);
    header->source = strdup(line);
    free(line);

    // parse metedata
    while(1){
        line = read_line(fd);
        // printf("here %s\n",line);
        header->source = realloc(header->source, (strlen(header->source) + strlen(line) + 1) * sizeof(char));
        strcat(header->source, line);
        if(line[0] == '\0' || (line[0] == '\r' && line[1] == '\n')){
            free(line);
            break;
        }
        if(strlen(line) != 0){

            char *line_split;
            char *line_full;
            line_full = line_split = strdup(line);
            char *key = strdup(strsep(&line_split, ":"));
            if(key == NULL){
                free(line_full);
                return NULL;
            }
            char *value = strsep(&line_split, "\r");
            if(value == NULL){
                free(key);
                free(line_full);
                return NULL;
            }
            char *p = value;
            while(*p == ' ') p++;
            value = strdup(p);
            // create the http_metadata_item object and
            // put the data in it
            http_metadata_item_t *item = (http_metadata_item_t*)malloc(sizeof(http_metadata_item_t));
            item->key = key;
            item->value = value; 
            TAILQ_INSERT_TAIL(&header->metadata_head, item, entries);
            free(line_full);
        }
        free(line);
    }
    return header;
}

/**
 * @function: pre_response(http_rquest_t* request)
 * @usage: for CONENCT response
 * @parameters: 
 * socket: send to which tunnel
 * buf : data_butter
 * buf_size : the size of the buffer
 * @return: the size of the sending data
 */
ssize_t send_data(int socket, const char* buf, size_t buf_size){
    ssize_t p = 0;
    while (p < buf_size){
        ssize_t ret = send(socket, buf+p,buf_size-p,0);
        if(ret > 0){
            p += ret;
        }else{
            return ret;
        }
    }
    return p;
}

/**
 * @function: pre_response(http_rquest_t* request)
 * @usage: for CONENCT response
 * @parameters: 
 * arg: the request
 * @return: the size of the ret
 */
ssize_t pre_response(http_rquest_t* request){
    const char response[] = "HTTP/1.1 200 Connection established\r\n"
    "Proxy-agent: ThinCar HTTP Proxy V1.0.\r\n\r\n";
    ssize_t ret = send_data(request->client,response,sizeof(response)-1);
    if (ret <= 0){ return 0;}
    return ret;
}


/**
 * @function: do_exchange(void *arg)
 * @usage: send and recv the data
 * @parameters: 
 * arg: the request
 * @return: void
 */
void * do_exchange(void *arg){
    http_rquest_t *request = (http_rquest_t *)arg;
    while(1)
    {
        char buffer[MAXSIZE] = {0};
        int ret = recv(request->client,buffer,MAXSIZE, 0);
        if (ret <= 0 ) {
            // if ret <= 0, means the transmit is end or some error occured
            thread_close[request->thread_used] = 1;
            return NULL;
        }       
        request->size += ret;
        //if socket did close, return null
        if(thread_close[request->thread_used] == 1) return NULL;
        
        ret = send_data(request->server,buffer,ret);
        if (ret <=0 ) {
            thread_close[request->thread_used] = 1;
            return NULL;
        }
    }
    return NULL;
}

/**
 * @function: exchange_data(void *arg)
 * @usage: Transmit http messages within the TLS connection
 * @parameters: 
 * arg: the request
 * @return: void
 */
void *exchange_data(void *arg){
    http_rquest_t *request1 = (http_rquest_t *)arg;
    thread_close[request1->thread_used] = 0;
    request1->status = 1;
    request1->isClient = -1;
    // create the transmit tunnel from server to client
    http_rquest_t *request2 = calloc(1, sizeof(http_rquest_t));
    request2->isClient = 1;
    request2->client = request1->server;
    request2->server = request1->client;
    request2->timeStart = request1->timeStart;
    request2->header = request1->header;
    request2->status = request1->status;
    request2->thread_used = request1->thread_used;
    int rc;
    // create two thread to do the transmission
    pthread_t threads[2];
    int *thread_ids = (int *) malloc(sizeof(int) * 2);

    thread_ids[0] = 1;
    thread_ids[1] = 2;
    pthread_create(&threads[0], NULL, do_exchange,request2);
    pthread_create(&threads[1], NULL, do_exchange,request1);
    
    while(1){
        usleep(100*1000);
        if(thread_close[request1->thread_used]){
            if(tele_info){
                // transmittion finished
                gettimeofday( &request2->timeEnd, NULL );
                // calculate time taken
                double runTime = (request2->timeEnd.tv_sec - request2->timeStart.tv_sec ) + (double)(request2->timeEnd.tv_usec -request2->timeStart.tv_usec)/1000000;
                // print the telemetry
                printf("Hostname: %s, Size: %d bytes, Time: %f sec\n", request2->header->host, request2->size, runTime);
            }
            pthread_cancel(threads[0]);
            pthread_cancel(threads[1]);
            break;
        }
    }  

    // close the socket and free the resource
    request1->status = 0;
    request2->status = request1->status;
    shutdown(request1->server, SHUT_RDWR);
    shutdown(request1->client, SHUT_RDWR);
    close(request1->server);
    close(request1->client);
    free(request2);
    request2 = NULL;
    return NULL;
}

/**
 * @function:connect_server(http_header_t *header)
 * @usage: Connect to the server
 * @parameters:
 * header: the header of the request
 * @return: the server socket
 */
int connect_server(http_header_t *header){
    http_metadata_item_t *item;
    char *value;
    char *host;
    TAILQ_FOREACH(item, &header->metadata_head, entries){
        if(strcmp(item->key, "Host") == 0)
        {
            value = strdup(item->value);
            host = strsep(&value, ":");
            header->host = host;   
        }
    }
    
    // If no port, go to find it in the search path
    if(value == NULL){
        char *port_str = strstr(header->search_path, host);
        if(port_str != NULL){
            port_str += strlen(host);
            char *port_str_temp = strstr(port_str, ":");
            if(port_str == port_str_temp){
                port_str_temp ++;
                value = strsep(&port_str_temp, "/\r");
            }
        }
        // if value is not designed, use the defalt value
        if(value == NULL){
            value = "80";
        }
    }

    // connect
    struct addrinfo hints, *servinfo = NULL;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    getaddrinfo(host, value, &hints, &servinfo);
    int server_socket = -1;
    for (struct addrinfo *i = servinfo; i != NULL; i = i->ai_next) {
        if ((server_socket = socket(i->ai_family, i->ai_socktype, i->ai_protocol)) == -1) {
            continue;
        }
        
        if (connect(server_socket, i->ai_addr, i->ai_addrlen) == -1) {
            close(server_socket);
            continue;
        }
        break;
    }
    
    // struct timeval on = {30,0};
    int on = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, &on, sizeof(on));
    freeaddrinfo(servinfo);
    return server_socket;
}

/**
 * @function:implement_proxy(void *t)
 * @usage: the function that each thread is implemented, waiting to
 * accept a new request if it is free.
 * @parameters:
 * t: thread id 
 * @return: void
 */
void implement_proxy(void *t){
    int *my_id = (int *)t;
    // keep waiting
    while(1){
        usleep(100*1000);
        if(thread_pool[*my_id] == 1){
            // get the correspinding request
            http_rquest_t *request = thread_request[*my_id];
            request->thread_used = *my_id;
            // read the header and store corresponding information
            request->header = read_header(request->client);
            // if the requesting host is in the black list, then return null
            int bl_line = 0;
            while (bl_line < fline){
                if (strlen(black_list[bl_line]) == 0) break;
                if(strstr(request->header->search_path, black_list[bl_line])){
                    printf("Request denied. %s is in the blacklist.\n",black_list[bl_line]);
                    thread_pool[request->thread_used] = 0;
                    close(request->client);
                    free_http_header(request->header);
                    free(request);
                    request = NULL;
                    return;
                }
                bl_line ++;
            }
            if(request->header == NULL){ 
                free(request);
                close(request->client);
                thread_pool[request->thread_used] = 0;
                return;
            }
            // connect the server, create TCP connection
            request->server = connect_server(request->header);
            if(request->header->method == CONNECT){
                pre_response(request);
            }else{
                send_data(request->server, request->header->source, strlen(request->header->source));
            }

            // exchange the buffer data from client to server and from server to client
            exchange_data(request);

            // after message transmitted free the thread
            thread_pool[request->thread_used] = 0;

            // free the resource.
            free_http_header(request->header);
            free(request);
            request = NULL;
        }
    }   
}

/**
 * @function:void http_proxy(int port)
 * @usage: start the proxy, allocate the request to the free thread
 * @parameters:
 * port: the port number that the proxy should listen to   
 * @return : void
 */

void http_proxy(int port){
    // indicate if it is already running
    if(_proxy_socket > 0){
        return;
    }
    _proxy_socket = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    int socklen = sizeof(addr);
    int on = 1;
    setsockopt(_proxy_socket, SOL_SOCKET, SO_RCVTIMEO, &on, sizeof(on));
    bind(_proxy_socket, (struct sockaddr*)&addr,socklen);
    listen(_proxy_socket, 100);
    if(_proxy_socket == -1){
        return;
    }

    int acceptSocket = -1;
    while(1){
        struct sockaddr_in addr;
        socklen_t addrLen = sizeof(addr);
        acceptSocket = accept(_proxy_socket,(struct sockaddr*)&addr, &addrLen);
        if(acceptSocket != -1){
            // initialize a request
            http_rquest_t *request = calloc(1, sizeof(http_rquest_t));
            request->size = 0;
            request->client = acceptSocket;
            setsockopt(acceptSocket, SOL_SOCKET, SO_RCVTIMEO, &on, sizeof(on));
            // find a free thread
            int thread_id = find_freeThread();
            // get the start time of the request
            gettimeofday(&request->timeStart, NULL );
            // add the request information into the array => match to the thread
            thread_request[thread_id] = request;
            // change the status of the used thread
            thread_pool[thread_id] = 1;
        }
    }
    return;
}

int main(int argc, const char * argv[]) {
    printf("HTTP proxy starts......\r\n");
    // indicate that the input number is correct.
    if(argc != 4){
        printf("[HTTPProxy] cannot start proxy with invalied arg count\r\n");
        return 0;
    }

    int port = atoi(argv[1]);
    tele_info = atoi(argv[2]); //if tele_info is 1 => provide telemetry; elif tele_info is 0 => do not provide.
    const char *bl = argv[3]; //the file name of the black list

    // read the blacklist
    FILE *fp = NULL;
    fp = fopen(bl, "r");
    while(!feof(fp)){
        char buff[255];
        fscanf(fp, "%s", buff);
        strcpy(black_list[fline], buff);
        fline = fline + 1;
    }
    fclose(fp);
    
    // multi-thread implementation:
    // using pthread to create 8 threads
    pthread_t threads_total[NUM_THREAD];
    int *thread_ids_total = (int *) malloc(sizeof(int) * NUM_THREAD);
    int rc;
    int i;
    for(i = 0; i<NUM_THREAD; i++){
        thread_ids_total[i] = i;
        rc = pthread_create(&threads_total[i], NULL, (void *)implement_proxy,(void*)&thread_ids_total[i]);
        if(rc){
            printf("ERROR: return code from pthread_create() is %d", rc);
            exit(1);
        }
    }

    // start the proxy
    http_proxy(port);
    return 0;
}
