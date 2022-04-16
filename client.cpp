#define _XOPEN_SOURCE 700
#include <iostream>
#include <sys/socket.h> 
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h> 
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <queue>
#include <errno.h>
#include "opencv2/opencv.hpp"
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <utility>

#define BUFF_SIZE 4096
#define VID_BUFF_SIZE 128
using namespace std;
using namespace cv;

typedef struct{
    int socket;
    int height;
    int width;
}play_data;
//typedef uchar;

void ls_dir(int socket);
void put_file(int socket, char filename[128]);
void get_file(int socket, char filename[128]);
void play_file(int socket, char filename[128]);
void *recv_to_buf(void *data);
unsigned int checksum(char *buf, int len);

pthread_mutex_t mutex;
queue< pair<uchar*, int> > vid_buf;
extern int errno;
int fin;
int stop_pushing;
char data_dir[20]  = "./client_data";

int main(int argc , char *argv[]){
    //create working directory
    struct stat st = {0};
    if (stat(data_dir, &st) == -1) {
        mkdir(data_dir, 0700);
    }
    //socket
    char *ip, *port_str;
    ip = strtok(argv[1], ":");
    port_str = strtok(NULL, ":");
    int port = atoi(port_str);
    int localSocket;
    localSocket = socket(AF_INET , SOCK_STREAM , 0);

    if (localSocket == -1){
        printf("Fail to create a socket.\n");
        return 0;
    }

    struct sockaddr_in info;
    bzero(&info,sizeof(info));

    info.sin_family = PF_INET;
    info.sin_addr.s_addr = inet_addr(ip);
    info.sin_port = htons(port);


    int err = connect(localSocket,(struct sockaddr *)&info,sizeof(info));
    if(err==-1){
        printf("Connection error\n");
        return 0;
    }

    //command
    string command;
    char act[20], filename[BUFF_SIZE], format_test[BUFF_SIZE];
    char cmpstr[3] = "-1";

    while(1){
        getline(cin, command);
        strcpy(act, "");
        strcpy(filename, cmpstr);
        strcpy(format_test, cmpstr);
        sscanf(command.c_str(), "%s %s %s", act, filename, format_test);
        if(strcmp(act, "ls") == 0){
            if(strcmp(filename, cmpstr) != 0){
                fprintf(stderr, "Command format error.\n");
                continue;
            }
            ls_dir(localSocket);
        }
        else if(strcmp(act, "put") == 0){
            if(strcmp(format_test, cmpstr) != 0){
                fprintf(stderr, "Command format error.\n");
                continue;
            }
            put_file(localSocket, filename);
        }
        else if(strcmp(act, "get") == 0){
            if(strcmp(format_test, cmpstr) != 0){
                fprintf(stderr, "Command format error.\n");
                continue;
            }
            get_file(localSocket, filename);
        }
        else if(strcmp(act, "play") == 0){
            if(strcmp(format_test, cmpstr) != 0){
                fprintf(stderr, "Command format error.\n");
                continue;
            }
            if(strcmp(&filename[strlen(filename)-4], ".mpg")!= 0){
                fprintf(stderr, "The \'%s\' is not a mpg file.\n", filename);
                continue;
            }
            play_file(localSocket, filename);
        }
        else{
            if(strcmp(act, "") != 0)
                fprintf(stderr, "Command \"%s\" not found.\n", act);
        }
    }
    printf("close Socket\n");
    close(localSocket);
    return 0;
}

void ls_dir(int socket){
    char buf[BUFF_SIZE];
    int check_n, send_n, recv_n;
    bzero(buf, BUFF_SIZE);
    sprintf(buf, "ls\n\n");
    send_n = send(socket, buf, strlen(buf)+1, 0);
    check_n = 1;
    while(check_n > 0){
        recv_n = recv(socket, &check_n, 4, MSG_WAITALL);
        if(check_n > 0){
            bzero(buf, BUFF_SIZE);
            recv_n = recv(socket, buf, check_n, MSG_WAITALL);
            printf("%s", buf);
        }
    }
    fflush(stdout);
    return;
}

void put_file(int socket, char filename[BUFF_SIZE]){
    char buf[BUFF_SIZE];
    char file_path[BUFF_SIZE];
    strcpy(file_path, data_dir);
    strcat(file_path, "/");
    strcat(file_path, filename);
    FILE *fp = fopen(file_path, "r");
    if(fp == NULL){
        printf("The \'%s\' doesn't exist.\n", filename);
        fflush(stdout);
        return;
    }
    int check_n, send_n, recv_n;
    unsigned int chksum;
    bzero(buf, BUFF_SIZE);
    sprintf(buf, "put\n%s\n", filename);
    send_n = send(socket, buf, strlen(buf)+1, 0);
    bzero(buf, BUFF_SIZE);
    recv_n = recv(socket, buf, BUFF_SIZE, 0);

    if(strcmp(buf, "StartStreaming\n") != 0){
        fprintf(stderr, "put file: error from server ...\n");
        return;
    }
    check_n = 1;
    while(check_n != 0){
        bzero(buf, BUFF_SIZE);
        check_n = fread(buf, 1, BUFF_SIZE, fp);
        if(check_n <= 0 && errno == EINTR){ //interrupted by signal
            check_n = 1;
            continue;
        }
        send_n = send(socket, &check_n, 4, 0);
        if(check_n > 0){
            send_n = send(socket, buf, check_n, 0);
            chksum = checksum(buf, check_n);
            send_n = send(socket, &chksum, 4, 0);
            bzero(buf, BUFF_SIZE);
            recv_n = recv(socket, buf, BUFF_SIZE, 0);
            if(strcmp(buf, "Checked\n") != 0){
                fseek(fp, -check_n, SEEK_CUR);
            }
        }
        else if(check_n == 0){
            fclose(fp);
            break;
        }
        else{   //other errors that we reupload the file
            fseek(fp, 0, SEEK_SET);
        }
    }
    fprintf(stderr, "done!\n");
    return;
}


void get_file(int socket, char filename[BUFF_SIZE]){
    char buf[BUFF_SIZE];
    int check_n, send_n, recv_n;
    bzero(buf, BUFF_SIZE);
    sprintf(buf, "get\n%s\n", filename);
    send_n = send(socket, buf, strlen(buf)+1, 0);
    bzero(buf, BUFF_SIZE);
    recv_n = recv(socket, buf, BUFF_SIZE, 0);
    if(strcmp(buf, "FileReady!\n") != 0){
        printf("The \'%s\' doesn't exist.\n", filename);
        fflush(stdout);
        return;
    }
    char file_path[256];
    strcpy(file_path, data_dir);
    strcat(file_path, "/");
    strcat(file_path, filename);
    FILE *fp;
    if((fp = fopen(file_path, "w")) == NULL){
        printf("Can't open \'%s\' at local.\n", filename);
        fflush(stdout);
        bzero(buf, BUFF_SIZE);
        sprintf(buf, "Error!\n");
        send_n = send(socket, buf, strlen(buf)+1, 0);
        return;
    }
    bzero(buf, BUFF_SIZE);
    sprintf(buf, "StartStreaming\n");
    send_n = send(socket, buf, strlen(buf)+1, 0);
    unsigned int chksum, chksum_svr;
    ssize_t w_n;
    check_n = 1;
    while(check_n != 0){
        recv_n = recv(socket, &check_n, 4, MSG_WAITALL);

        if(check_n > 0){
            bzero(buf, BUFF_SIZE);
            recv_n = recv(socket, buf, check_n, MSG_WAITALL);
            chksum = checksum(buf, check_n);
            recv_n = recv(socket, &chksum_svr, 4, MSG_WAITALL);
            if(chksum == chksum_svr){
                while(1){
                    w_n = fwrite(buf, 1, check_n, fp);
                    if(w_n >= 0) break;
                }
                bzero(buf, BUFF_SIZE);
                sprintf(buf, "Checked\n");
                send_n = send(socket, buf, strlen(buf)+1, 0);
            }
            else{
                bzero(buf, BUFF_SIZE);
                sprintf(buf, "ChecksumFailed\n");
                send_n = send(socket, buf, strlen(buf)+1, 0);
            }
        }
        else if(check_n == 0){
            fclose(fp);
            break;
        }
        else{
            fseek(fp, 0, SEEK_SET);
        }
    }
    printf("done!\n");
    fflush(stdout);
    return;
}

void play_file(int socket, char filename[BUFF_SIZE]){
    char buf[BUFF_SIZE];
    int check_n, send_n, recv_n;
    bzero(buf, BUFF_SIZE);
    sprintf(buf, "play\n%s\n", filename);
    send_n = send(socket, buf, strlen(buf)+1, 0);
    
    bzero(buf, BUFF_SIZE);
    recv_n = recv(socket, buf, BUFF_SIZE, 0);
    if(strcmp(buf, "FileOpened\n") != 0){
        printf("The \'%s\' doesn't exist.\n", filename);
        fflush(stdout);
        return;
    }
    bzero(buf, BUFF_SIZE);
    sprintf(buf, "Resolution\n");
    send_n = send(socket, buf, strlen(buf)+1, 0);
    
    int width, height, imgSize;
    recv_n = recv(socket, &width, 4, MSG_WAITALL);
    recv_n = recv(socket, &height, 4, MSG_WAITALL);
    Mat imgClient;
    imgClient = Mat::zeros(height, width, CV_8UC3);
    if(!imgClient.isContinuous()){
         imgClient = imgClient.clone();
    }
    pthread_t t;
    play_data pd;
    pd.socket = socket;
    pd.height = height;
    pd.width = width;
    fin = 0;
    stop_pushing = 0;
    pthread_create(&t, NULL, recv_to_buf, (void *)&pd);
    bzero(buf, BUFF_SIZE);
    sprintf(buf, "StartStreaming\n");
    send_n = send(socket, buf, strlen(buf)+1, 0);
    pair<uchar*, int> imgData;
    namedWindow("GetFocus", WINDOW_AUTOSIZE);
    imshow("GetFocus", imgClient);
    setWindowProperty("GetFocus", CV_WND_PROP_FULLSCREEN, CV_WINDOW_FULLSCREEN);
    setWindowProperty("GetFocus", CV_WND_PROP_FULLSCREEN, CV_WINDOW_NORMAL);
    destroyWindow("GetFocus");
    while(fin == 0){
        while(!vid_buf.empty()){
            pthread_mutex_lock(&mutex);
            imgData = vid_buf.front();
            vid_buf.pop();
            pthread_mutex_unlock(&mutex);
            memcpy(imgClient.data, imgData.first, imgData.second);
            imshow(filename, imgClient);
            if(imgData.first != NULL){
                free(imgData.first);
                imgData.first = NULL;
            }
            char c = (char)waitKey(33.3333);
            if(c == 27){
                fin = 1;
                if(!vid_buf.empty()){
                    pthread_mutex_lock(&mutex);
                    imgData = vid_buf.front();
                    if(imgData.first != NULL){
                        free(imgData.first);
                        imgData.first = NULL;
                    }
                    vid_buf.pop();
                    pthread_mutex_unlock(&mutex);
                }
                break;
            }
        }
    }

    destroyAllWindows();
    if(imgData.first != NULL){
        free(imgData.first);
        imgData.first = NULL;
    }
    //clean the buffer
    while(stop_pushing == 0);
    while(!vid_buf.empty()){
            imgData = vid_buf.front();
            vid_buf.pop();
            if(imgData.first != NULL){
                free(imgData.first);
                imgData.first = NULL;
            }
    }
    pthread_join(t, NULL);
    fprintf(stderr, "finish playing\n");
    return;
}

void *recv_to_buf(void *data){
    
    play_data *pd = (play_data *)data;
    int height = pd->height;
    int width = pd->width;
    int socket = pd->socket;
    int imgSize;
    int recv_n, send_n, check_n;
    uchar *buf = NULL;
    Mat img;
    img = Mat::zeros(height, width, CV_8UC3);
    if(!img.isContinuous()){
         img = img.clone();
    }
    check_n = 1;
    imgSize = 1;
    while(imgSize >= 0){
        recv_n = recv(socket, &imgSize, 4, MSG_WAITALL);
        if(imgSize > 0){
            buf = (uchar *)malloc(imgSize);
            bzero(buf, imgSize);
            recv_n = recv(socket, buf, imgSize, MSG_WAITALL);
            if(recv_n == imgSize){
                while(vid_buf.size() >= VID_BUFF_SIZE);
                pthread_mutex_lock(&mutex);
                vid_buf.push(make_pair(buf, imgSize));
                pthread_mutex_unlock(&mutex);
            }
        }
        else if(imgSize == -1){
            fin = 1;
            stop_pushing = 1;
            break;
        }
        if(fin == 1){
            check_n = 0;
            stop_pushing = 1;
            send_n = send(socket, &check_n, 4, 0);
            recv_n = recv(socket, &imgSize, 4, MSG_WAITALL);
            break;
        }
        else{
            send_n = send(socket, &check_n, 4, 0);
        }
    }
    pthread_exit(NULL);
}

unsigned int checksum(char *buf, int len){
    unsigned int chksum = 0, i;
    for(i = 0; i < len; i++)
        chksum += (unsigned char)buf[i];
    return chksum;
}

