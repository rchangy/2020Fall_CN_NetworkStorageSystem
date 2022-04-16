#define _XOPEN_SOURCE 700
#include <iostream>
#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h> 
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <queue>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include "opencv2/opencv.hpp"
#include <sstream>
#include <dirent.h>
#include <vector>

#define BUFF_SIZE 4096
#define MAX_CONN 1024
#define VID_BUFF_SIZE 128
#define VID_BUFF_NUM 128

using namespace std;
using namespace cv;

typedef struct{
    pthread_t t;
    int id;
}vid_data;

typedef struct{
    char host[512];
    int conn_fd;
    vid_data *vd;
    FILE *fp; //fp for put/get
    int pending_comm;
    char *filename;
} Client;




int put_file_init(Client *client, char *filename);
int get_file_init(Client *client, char *filename);
int play_vid_init(Client *client, char *filename);
int put_file(Client *client);
int get_file(Client *client);
int play_vid(Client *client);
void *push_to_buf(void *data);
unsigned int checksum(char *buf, int len);
//int sigpipe_handle();

pthread_mutex_t mutex[VID_BUFF_NUM];
int cur_vid_num;
queue<Mat> vid_buf[VID_BUFF_NUM];
int fin[VID_BUFF_NUM];
int stop_pushing[VID_BUFF_NUM];
VideoCapture cap[VID_BUFF_NUM];
Mat img_push[VID_BUFF_NUM];
Mat img_pop[VID_BUFF_NUM];

extern int errno;
char data_dir[20] = "./server_data";
char buf[BUFF_SIZE];
int test_flag = 0;
int main(int argc, char** argv){
    struct sigaction act;
    act.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &act, NULL);
    //create working directory
    struct stat st = {0};
    if (stat(data_dir, &st) == -1) {
        mkdir(data_dir, 0700);
    }

    DIR *dp;
    struct dirent *ep;
    int filename_len;

    // client table
    int maxfd = getdtablesize();
    Client* client = NULL;
    client = (Client*) malloc(sizeof(Client)*maxfd);
    if (client == NULL) {
        fprintf(stderr, "out of memory allocating all requests");
    }
    for (int i = 0; i < maxfd; i++) {
        client[i].conn_fd = -1;
    }

    //local socket
    int localSocket, remoteSocket, port = atoi(argv[1]);                               

    struct  sockaddr_in localAddr,remoteAddr;
          
    int addrLen = sizeof(struct sockaddr_in);  

    localSocket = socket(AF_INET , SOCK_STREAM , 0);
    
    if (localSocket == -1){
        printf("socket() call failed!!");
        return 0;
    }

    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = htons(port);

    //char Message[BUFF_SIZE] = {};

        
    if( bind(localSocket,(struct sockaddr *)&localAddr , sizeof(localAddr)) < 0) {
        printf("Can't bind() socket");
        return 0;
    }
    
    listen(localSocket , MAX_CONN);

    client[localSocket].conn_fd = localSocket;
    gethostname(client[localSocket].host, sizeof(client[localSocket].host));

    fprintf(stderr, "Waiting for connections...\nServer Port:%d\n", port);

    //fdset
    fd_set rfd, request, vfd, vid, pfd, put, gfd, get, pending;
    FD_ZERO(&rfd);
    FD_ZERO(&request);
    FD_ZERO(&vfd);
    FD_ZERO(&vid);
    FD_ZERO(&pfd);
    FD_ZERO(&put);
    FD_ZERO(&gfd);
    FD_ZERO(&get);
    FD_ZERO(&pending);
    FD_SET(localSocket, &request);
    int working, pend_n = 0;
    int conn_fd;
    //timeout
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    //ls
    vector<int> ls_list;
    ls_list.reserve(maxfd);

    char command[128] ,filename[128];
    int recv_n, send_n;
    char buf[BUFF_SIZE];
    int ret;
    cur_vid_num = 0;
    for(int i = 0; i < VID_BUFF_NUM; i++){
        fin[i] = -1;
    }
    while(1){
        ls_list.clear();
        memcpy(&rfd, &request, sizeof(request));
        working = select(maxfd, &rfd, NULL, NULL, &timeout);
        memcpy(&pfd, &put, sizeof(put));
        working += select(maxfd, &pfd, NULL, NULL, &timeout);
        memcpy(&gfd, &get, sizeof(get));
        working += select(maxfd, NULL, &gfd, NULL, &timeout);
        memcpy(&vfd, &vid, sizeof(vid));
        working += select(maxfd, NULL, &vfd, NULL, &timeout);
        for(int i = 0; i < maxfd && working > 0; i++){
            if(FD_ISSET(i, &rfd)){
                working--;
                if(i == localSocket){       //new connection
                    conn_fd = accept(localSocket, (struct sockaddr *)&remoteAddr, (socklen_t*)&addrLen);
                    if (conn_fd < 0) {
                        if (errno == EINTR || errno == EAGAIN) continue;  // try again 
                        if (errno == ENFILE) {
                            (void) fprintf(stderr, "out of file descriptor table ... (maxconn %d)\n", maxfd);
                            continue;
                        }
                    }
                    client[conn_fd].conn_fd = conn_fd;
                    client[conn_fd].vd = NULL;
                    client[conn_fd].fp = NULL;
                    client[conn_fd].pending_comm = -1;
                    client[conn_fd].filename = NULL;
                    strcpy(client[conn_fd].host, inet_ntoa(remoteAddr.sin_addr));
                    fprintf(stderr, "getting a new request... fd %d from %s\n", conn_fd, client[conn_fd].host);
                    FD_SET(conn_fd, &request);
                }
                else{
                    //read request
                    bzero(buf, BUFF_SIZE);
                    recv_n  = recv(i, buf, BUFF_SIZE, 0);
                    if(recv_n == 0){
                        //client exit
                        FD_CLR(i, &request);
                        client[i].conn_fd = -1;
                        fprintf(stderr, "socket fd %d closed\n", i);
                    }
                    else{
                        sscanf(buf, "%s\n%s\n", command, filename);
                        fprintf(stderr, "received command \'%s\' from client %d\n", command, i);
                        if(strcmp(command, "ls") == 0){
                            ls_list.push_back(i);
                        }
                        else if(strcmp(command, "put") == 0){
                            ret = put_file_init(&client[i], filename);
                            if(ret == 1){
                                FD_SET(i, &put);
                                FD_CLR(i, &request);
                            }
                            else if(ret == 2){
                                FD_SET(i, &pending);
                                FD_CLR(i, &request);
                                pend_n++;
                                client[i].pending_comm = 1;
                                client[i].filename = (char *)malloc(strlen(filename)+1);
                                strcpy(client[i].filename, filename);
                            }
                        }
                        else if(strcmp(command, "get") == 0){
                            ret = get_file_init(&client[i], filename);
                            if(ret == 1){
                                FD_SET(i, &get);
                                FD_CLR(i, &request);
                            }
                            else if(ret == 2){
                                FD_SET(i, &pending);
                                FD_CLR(i, &request);
                                pend_n++;
                                client[i].pending_comm = 2;
                                client[i].filename = (char *)malloc(strlen(filename)+1);
                                strcpy(client[i].filename, filename);
                            }
                        }
                        else if(strcmp(command, "play") == 0){
                            if(cur_vid_num >= VID_BUFF_NUM) ret = 2;
                            else ret = play_vid_init(&client[i], filename);
                            if(ret == 1){
                                FD_SET(i, &vid);
                                FD_CLR(i, &request);
                            }
                            else if(ret == 2){
                                FD_SET(i, &pending);
                                FD_CLR(i, &request);
                                pend_n++;
                                client[i].pending_comm = 3;
                                client[i].filename = (char *)malloc(strlen(filename)+1);
                                strcpy(client[i].filename, filename);
                                fprintf(stderr, "go to pending\n");
                            }

                        }
                    }
                }
            }
            else if(FD_ISSET(i, &pfd)){
                working--;
                ret = put_file(&client[i]);
                if(ret == 0){
                    FD_CLR(i, &put);
                    FD_SET(i, &request);
                    fprintf(stderr, "done!\n");
                }
                if(ret == -1){
                    FD_CLR(i, &put);
                    FD_SET(i, &request);
                    fprintf(stderr, "stop put file due to closed socket %d\n", i);
                }
            }
            else if(FD_ISSET(i, &gfd)){
                working--;
                ret = get_file(&client[i]);
                if(ret == 0){
                    FD_CLR(i, &get);
                    FD_SET(i, &request);
                    fprintf(stderr, "done!\n");
                }
                if(ret == -1){
                    FD_CLR(i, &get);
                    FD_SET(i, &request);
                    fprintf(stderr, "stop get file due to closed socket %d\n", i);
                }
            }
            else if(FD_ISSET(i, &vfd)){
                working--;
                ret = play_vid(&client[i]);
                if(ret == 0 ){
                    FD_CLR(i, &vid);
                    FD_SET(i, &request);
                    fprintf(stderr, "done!\n");
                }
                if(ret == -1){
                    FD_CLR(i, &vid);
                    FD_SET(i, &request);
                    fprintf(stderr, "stop play vid due to closed socket %d\n", i);
                }
            }
        }
        //pending
        if(pend_n > 0){
            for(int i = 0; i < maxfd; i++){
                if(FD_ISSET(i, &pending)){
                    if(client[i].pending_comm == 1){
                        ret = put_file_init(&client[i], client[i].filename);
                        if(ret != 2){
                            pend_n--;
                            client[i].pending_comm = -1;
                            free(client[i].filename);
                            if(ret == 1){
                                FD_SET(i, &put);
                                FD_CLR(i, &pending);
                            }
                            else if(ret == 0 || ret == -1){
                                FD_CLR(i, &pending);
                                FD_SET(i, &request);
                            }
                        }
                    }
                    else if(client[i].pending_comm == 2){
                        ret = get_file_init(&client[i], client[i].filename);
                        if(ret != 2){
                            pend_n--;
                            client[i].pending_comm = -1;
                            free(client[i].filename);
                            if(ret == 1){
                                FD_SET(i, &get);
                                FD_CLR(i, &pending);
                            }
                            else if(ret == 0  || ret == -1){
                                FD_CLR(i, &pending);
                                FD_SET(i, &request);
                            }
                        }
                    }
                    else if(client[i].pending_comm == 3){
                        fprintf(stderr, "try exit pending\n");
                        if(cur_vid_num >= VID_BUFF_NUM) ret = 2;
                        else ret = play_vid_init(&client[i], client[i].filename);
                        if(ret != 2){
                            fprintf(stderr, "start get\n");
                            pend_n--;
                            client[i].pending_comm = -1;
                            free(client[i].filename);
                            if(ret == 1){
                                FD_SET(i, &vid);
                                FD_CLR(i, &pending);
                            }
                            else if(ret == 0  || ret == -1){
                                FD_CLR(i, &pending);
                                FD_SET(i, &request);
                            }
                        }
                    }
                }
            }
        }
        if(ls_list.size() > 0){
            dp = opendir (data_dir);
            if (dp != NULL){
                bzero(buf, BUFF_SIZE);
                filename_len = 0;
                while (ep = readdir (dp)){
                    if(filename_len + strlen(ep->d_name) + 1 >= BUFF_SIZE){
                        if(filename_len > 0){
                            for(int j = 0; j < ls_list.size(); j++){
                                send_n = send(ls_list[j], &filename_len, 4, 0);
                                if(send_n == -1 && errno == EPIPE){
                                    ls_list.erase(ls_list.begin()+j);
                                    continue;
                                }
                                send_n = send(ls_list[j], buf, strlen(buf), 0);
                                if(send_n == -1 && errno == EPIPE){
                                    ls_list.erase(ls_list.begin()+j);
                                }
                            }
                            bzero(buf, BUFF_SIZE);
                            filename_len = 0;
                        }
                        if((filename_len = strlen(ep->d_name) + 1) >= BUFF_SIZE){
                            int iter = filename_len;
                            while(iter > 0){
                                bzero(buf, BUFF_SIZE);
                                int s = min(BUFF_SIZE, iter);
                                memcpy(buf, &ep->d_name[filename_len - iter], s);
                                iter -= s;
                                for(int j = 0; j < ls_list.size(); j++){
                                    send_n = send(ls_list[j], &s, 4, 0);
                                    send_n = send(ls_list[j], buf, s, 0);
                                    if(send_n == -1 && errno == EPIPE){
                                        ls_list.erase(ls_list.begin()+j);
                                    }
                                }
                            }
                            bzero(buf, BUFF_SIZE);
                            strcpy(buf, "\n");
                            iter = 2;
                            for(int j = 0; j < ls_list.size(); j++){
                                send_n = send(ls_list[j], &iter, 4, 0);
                                send_n = send(ls_list[j], buf, strlen(buf)+1, 0);
                                if(send_n == -1 && errno == EPIPE){
                                    ls_list.erase(ls_list.begin()+j);\
                                }
                            }
                            filename_len = 0;
                        }
                        else{
                            strcat(buf, ep->d_name);
                            strcat(buf, "\n");
                            filename_len = strlen(ep->d_name) + 1;
                        }
                    }
                    else{
                        strcat(buf, ep->d_name);
                        strcat(buf, "\n");
                        filename_len += strlen(ep->d_name) + 1;
                    }
                }
                if(filename_len > 0){
                    for(int j = 0; j < ls_list.size(); j++){
                        send_n = send(ls_list[j], &filename_len, 4, 0);
                        send_n = send(ls_list[j], buf, strlen(buf), 0);
                        if(send_n == -1 && errno == EPIPE){
                            ls_list.erase(ls_list.begin()+j);   
                        }
                    }
                }
                (void) closedir (dp);
                filename_len = 0;
                for(int j = 0; j < ls_list.size(); j++){
                    send_n = send(ls_list[j], &filename_len, 4, 0);
                }
            }
        }
    }
    return 0;
}

int put_file_init(Client *client, char *filename){
    int socket = client->conn_fd;
    int send_n;
    char file_path[BUFF_SIZE];
    strcpy(file_path, data_dir);
    strcat(file_path, "/");
    strcat(file_path, filename);
    if((client->fp = fopen(file_path, "w")) == NULL){
        if(errno == EMFILE) return 2;
        fprintf(stderr, "client %d: OpenFileFailed\n", client->conn_fd);
        bzero(buf, BUFF_SIZE);
        sprintf(buf, "OpenFileFailed\n");
        send_n = send(socket, buf, strlen(buf)+1, 0);
        return 0;
    }
    bzero(buf, BUFF_SIZE);
    sprintf(buf, "StartStreaming\n");
    send_n = send(socket, buf, strlen(buf)+1, 0);
    if(send_n == -1 && errno == EPIPE) return -1;
    return 1;
}
int get_file_init(Client *client, char *filename){
    int send_n, recv_n;
    int socket = client->conn_fd;
    char file_path[BUFF_SIZE];
    strcpy(file_path, data_dir);
    strcat(file_path, "/");
    strcat(file_path, filename);
    if((client -> fp = fopen(file_path, "r")) == NULL){
        if(errno == EMFILE) return 2;
        fprintf(stderr, "client %d: file \"%s\" not found!\n", client->conn_fd, file_path);
        bzero(buf, BUFF_SIZE);
        sprintf(buf, "Error!\n");
        send_n = send(socket, buf, strlen(buf)+1, 0);
        return 0;
    }
    bzero(buf, BUFF_SIZE);
    sprintf(buf, "FileReady!\n");
    send_n = send(socket, buf, strlen(buf)+1, 0);
    if(send_n == -1 && errno == EPIPE) return -1;
    bzero(buf, BUFF_SIZE);
    recv_n = recv(socket, buf, BUFF_SIZE, 0);
    if(strcmp(buf, "StartStreaming\n") != 0) return 0;
    return 1;
}
int play_vid_init(Client *client, char *filename){
    vid_data *vd = (vid_data *)malloc(sizeof(vid_data));
    int id;
    for(id = 0; id < VID_BUFF_NUM; id++){
        if(fin[id] == -1){
            vd->id = id;
            break;
        }
    }
    int socket = client->conn_fd;
    int send_n, recv_n, check_n;
    char file_path[BUFF_SIZE];
    strcpy(file_path, data_dir);
    strcat(file_path, "/");
    strcat(file_path, filename);
    //open vid
    cap[id].open(file_path);
    if(!cap[id].isOpened()){
        bzero(buf, BUFF_SIZE);
        sprintf(buf, "Error!\n");
        fprintf(stderr, "client %d: Can't open file!\n", client->conn_fd);
        send_n = send(socket, buf, strlen(buf)+1, 0);
        free(vd);
        return 0;
    }
    /*
    int ex = static_cast<int>(cap[id].get(CV_CAP_PROP_FOURCC));
    char EXT[] = {ex & 0XFF , (ex & 0XFF00) >> 8,(ex & 0XFF0000) >> 16,(ex & 0XFF000000) >> 24, 0};
    fprintf(stderr, "form\n%s\n", EXT);
    */
    bzero(buf, BUFF_SIZE);
    sprintf(buf, "FileOpened\n");
    send_n = send(socket, buf, strlen(buf)+1, 0);
    if(send_n == -1 && errno == EPIPE){
        cap[id].release();
        free(vd);
        return -1;
    }
    bzero(buf, BUFF_SIZE);
    recv_n = recv(socket, buf, BUFF_SIZE, 0);
    if(strcmp(buf, "Resolution\n") != 0){
        cap[id].release();
        free(vd);
        return 0;
    }
    //resolution
    int width = cap[id].get(CV_CAP_PROP_FRAME_WIDTH);
    int height = cap[id].get(CV_CAP_PROP_FRAME_HEIGHT);
    img_push[id] = Mat::zeros(height,width, CV_8UC3);
    if(!img_push[id].isContinuous()){
         img_push[id] = img_push[id].clone();
    }
    img_pop[id] = Mat::zeros(height,width, CV_8UC3);
    if(!img_pop[id].isContinuous()){
         img_pop[id] = img_pop[id].clone();
    }
    send_n = send(socket, &width, 4, 0);
    send_n = send(socket, &height, 4, 0);
    if(send_n == -1 && errno == EPIPE){
        cap[id].release();
        free(vd);
        return -1;
    }
    bzero(buf, BUFF_SIZE);
    recv_n = recv(socket, buf, BUFF_SIZE, 0);
    if(strcmp(buf, "StartStreaming\n") != 0){
        cap[id].release();
        free(vd);
        return 0;
    }
    fin[id] = 0;
    stop_pushing[id] = 0;
    client->vd = vd;
    pthread_create(&vd->t, NULL, push_to_buf, (void *)vd);
    return 1;
}

int put_file(Client *client){
    int socket = client->conn_fd;
    int send_n, recv_n;
    int check_n = 0;
    ssize_t w_n;
    unsigned int chksum, chksum_cli;
    //get one chunk
    recv_n = recv(socket, &check_n, 4, 0);
    if(check_n > 0){
        bzero(buf, BUFF_SIZE);
        recv_n = recv(socket, buf, check_n, MSG_WAITALL);
        chksum = checksum(buf, check_n);
        recv_n = recv(socket, &chksum_cli, 4, MSG_WAITALL);
        if(chksum == chksum_cli){
            while(1){
                w_n = fwrite(buf, 1, check_n, client->fp);
                if(w_n > 0) break;
            }
            bzero(buf, BUFF_SIZE);
            sprintf(buf, "Checked\n");
            send_n = send(socket, buf, strlen(buf)+1, 0);
            if(send_n == -1 && errno == EPIPE){
                fclose(client->fp);
                client->fp = NULL;
                return -1;
            }
        }
        else{
            bzero(buf, BUFF_SIZE);
            sprintf(buf, "ChecksumFailed\n");
            send_n = send(socket, buf, strlen(buf)+1, 0);
            if(send_n == -1 && errno == EPIPE){
                fclose(client->fp);
                client->fp = NULL;
                return -1;
            }
        }
    }
    else if(check_n == 0){
        fclose(client->fp);
        client->fp = NULL;
    }
    else{
        fseek(client->fp, 0, SEEK_SET);
    }
    return check_n;
}

int get_file(Client *client){
    int socket = client->conn_fd;
    int send_n, recv_n, check_n;
    unsigned chksum;
    bzero(buf, BUFF_SIZE);
    check_n = fread(buf, 1, BUFF_SIZE, client->fp);
    while(check_n < 0 && errno == EINTR) check_n = fread(buf, 1, BUFF_SIZE, client->fp);
    send_n = send(socket, &check_n, 4, 0);
    if(send_n == -1 && errno == EPIPE){
        fclose(client->fp);
        client->fp = NULL;
        return -1;
    }
    if(check_n > 0){
        send_n = send(socket, buf, check_n, 0);
        chksum = checksum(buf, check_n);
        send_n = send(socket, &chksum, 4, 0);
        if(send_n == -1 && errno == EPIPE){
            fclose(client->fp);
            client->fp = NULL;
            return -1;
        }
        bzero(buf, BUFF_SIZE);
        recv_n = recv(socket, buf, BUFF_SIZE, 0);
        if(strcmp(buf, "Checked\n") != 0){
            fseek(client->fp, -check_n, SEEK_CUR);
        }
    }
    else if(check_n == 0){
        fclose(client->fp);
        client->fp = NULL;
    }
    else{
        fseek(client->fp, 0, SEEK_SET);
    }
    return check_n;
}



int play_vid(Client *client){
    int socket = client->conn_fd;
    int id = client->vd->id;
    vid_data *vd = client->vd;
    int send_n, recv_n, check_n;
    uchar *buffer;
    int imgSize;
    if(vid_buf[id].empty()){
        if(fin[id] == 1){
            imgSize = -1;
            send_n = send(socket, &imgSize, 4, 0);
            pthread_join(vd->t, NULL);
            free(vd);
            client->vd = NULL;
            fin[id] = -1;
            stop_pushing[id] = -1;
            return 0;
        }
        return 1;
    }
    pthread_mutex_lock(&mutex[id]);
    img_pop[id] = vid_buf[id].front();
    vid_buf[id].pop();
    pthread_mutex_unlock(&mutex[id]);
    imgSize = img_pop[id].total() * img_pop[id].elemSize();
    send_n = send(socket, &imgSize, 4, 0);
    if(send_n == -1 && errno == EPIPE){
        fin[id] = 1;
        if(!vid_buf[id].empty()){
            pthread_mutex_lock(&mutex[id]);
            vid_buf[id].pop();
            pthread_mutex_unlock(&mutex[id]);
        }
        while(stop_pushing[id] == 0);
        while(!vid_buf[id].empty()) vid_buf[id].pop();
        pthread_join(vd->t, NULL);
        free(vd);
        client->vd = NULL;
        fin[id] = -1;
        stop_pushing[id] = -1;
        return -1;
    }
    if(imgSize > 0){
        buffer = (uchar *)malloc(imgSize);
        bzero(buffer, imgSize);
        memcpy(buffer, img_pop[id].data, imgSize);
        send_n = send(socket, buffer, imgSize, 0);
        if(buffer != NULL){
            free(buffer);
            buffer = NULL;
        }
        if(send_n == -1 && errno == EPIPE){
            fin[id] = 1;
            if(!vid_buf[id].empty()){
                pthread_mutex_lock(&mutex[id]);
                vid_buf[id].pop();
                pthread_mutex_unlock(&mutex[id]);
            }
            while(stop_pushing[id] == 0);
            while(!vid_buf[id].empty()) vid_buf[id].pop();
            pthread_join(vd->t, NULL);
            free(vd);
            client->vd = NULL;
            fin[id] = -1;
            stop_pushing[id] = -1;
            return -1;
        }
    }
    recv_n = recv(socket, &check_n, 4, MSG_WAITALL);
    if(check_n == 0){
        imgSize = -1;
        send_n = send(socket, &imgSize, 4, 0);
        fprintf(stderr, "client %d: stop streaming\n", client->conn_fd);
        fin[id] = 1;
        if(!vid_buf[id].empty()){
            pthread_mutex_lock(&mutex[id]);
            vid_buf[id].pop();
            pthread_mutex_unlock(&mutex[id]);
        }
        while(stop_pushing[id] == 0);
        while(!vid_buf[id].empty()) vid_buf[id].pop();
        pthread_join(vd->t, NULL);
        free(vd);
        client->vd = NULL;
        fin[id] = -1;
        stop_pushing[id] = -1;
        return 0;
    }

    return 1;
}

void *push_to_buf(void *data){
    vid_data *vd = (vid_data *)data;
    int id = vd->id;
    while(fin[id] == 0){
        cap[id] >> img_push[id];
        if(img_push[id].empty()){
            break;
        }
        while(vid_buf[id].size() >= VID_BUFF_SIZE);
        pthread_mutex_lock(&mutex[id]);
        vid_buf[id].push(img_push[id]);
        pthread_mutex_unlock(&mutex[id]);
    }

    fin[id] = 1;
    stop_pushing[id] = 1;
    cap[id].release();
    
    pthread_exit(NULL);
}

unsigned int checksum(char *buf, int len){
    unsigned int chksum = 0, i;
    for(i = 0; i < len; i++){
        chksum += (unsigned char)buf[i];
    }
    return chksum;
}

