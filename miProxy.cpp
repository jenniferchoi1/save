#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <algorithm>
#include <iostream>

#define MAXCLIENT 8

using namespace std;

int getSocket(int listen_port) {

    int server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket < 0){
        perror("Error: Socket failed");
        return -1;
    }

    struct sockaddr_in proxy_address;
    proxy_address.sin_family = AF_INET;
    proxy_address.sin_addr.s_addr = INADDR_ANY;
    proxy_address.sin_port = htons(listen_port);

    int success_bind = ::bind(server_socket, (struct sockaddr *)&proxy_address, sizeof(proxy_address));

    if (success_bind < 0){  
        perror("Error: bind failed");
        return -1;
    }

    int success_listen = listen(server_socket, MAXCLIENT);
    if (success_listen < 0){
        return -1;
    }

    return server_socket;

}

int newBitrate(double throughput, vector<int> bitrates){
    int new_bitrate;
    throughput /= 1.5;
    int smallest_bitrate = bitrates.front();
    for (int bit : bitrates){
        if (bit <= throughput){
            new_bitrate = bit;
        } else {
            new_bitrate = smallest_bitrate;
            break;
        }
        smallest_bitrate = bit;
    }

    return new_bitrate;
}

string getChunkName(string request){
    string name;
    int index_get = request.find("GET");
    int index_end = request.find(' ', index_get + 4);
    int index_vod = request.rfind("vod", index_end);
    name = request.substr(index_vod + 4, index_end - index_vod - 4);

    return name;
}

string getResponseWithNewChunkName(string request, string newname){
    string name;
    int pos_start = request.find("GET") + 4;
    int pos_end = request.find(' ', pos_start);
    pos_start = request.rfind("vod", pos_end);
    name = request.replace(pos_start + 4, pos_end - pos_start - 4, newname);
    return name;
}

bool checkVideoData(string str){
    if (str.find("Seg") == -1){
        return false;
    }
    if (str.find("Frag") == -1){
        return false;
    }
    if (str.find("Frag") > str.find("Seg")){
        return true;
    }
    return false;
}

string getValue(string str, string key)
{
    int key_index = str.find(key);
    int space_index = str.find(' ', key_index);
    int end = str.find('\n', key_index);
    return str.substr(space_index + 1, end - space_index - 1);
}

string getIp(int fd){
    char client_ip[INET_ADDRSTRLEN];
    struct sockaddr_in client_addr;
    int addrlen = sizeof(client_addr);
    getpeername(fd, (struct sockaddr *)&client_addr, (socklen_t *)&addrlen);
    inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
    return (string)client_ip;
}

string recvResponse(int server_sd)
{
    string response = "";
    char buffer;
    while (true){
        int byte_recv = recv(server_sd, &buffer, 1, 0);

        if (byte_recv < 0){
            perror("Error: recv failed");
            exit(-1);
        }
        response += buffer;
        if (response.size() >= 4){
            if (response.substr(response.size() - 4) == "\r\n\r\n")
                break;
        }
    }
    return response;
}

double totalTime (struct timeval t1, struct timeval t2){
    return t2.tv_sec - t1.tv_sec + (t2.tv_usec - t1.tv_usec) / 1000000.0;
}

double getThroughput (int length, double total_time){
    return (length / 1000) / total_time * 8;
}

int main(int argc, char const *argv[]){

    if(argc != 6){
        printf("Usage: %s --nodns <listen-port> <www-ip> <alpha> <log>\n",argv[0]);
        return -1;
    }

    int listen_port = atoi(argv[2]);
    char *www_ip = (char *)argv[3];
    float alpha = atof(argv[4]);
    char *log_path = (char *)argv[5]; 

    // Get server socket
    int server_socket = getSocket(listen_port);

    fd_set readfds;

    vector<int> fds;
    vector<int> bitrates;
    unordered_map<int, double> throughput;

    string browser_ip;
    ofstream logfile;
    logfile.open(log_path);

    while (true){

        FD_ZERO(&readfds);
        FD_SET(server_socket, &readfds);

        for (int i = 0; i < (int)fds.size(); i++)
        {
            FD_SET(fds[i], &readfds);
        }
        int max_fd = 0;
        if (fds.size() > 0)
        {
            max_fd = *max_element(fds.begin(), fds.end());
        }
        max_fd = max(max_fd, server_socket);

        int error_select = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (error_select < 0){
            perror("Error: Select failed");
            return (-1);
        }

        if (FD_ISSET(server_socket, &readfds)){
            int client_sd = accept(server_socket, NULL, NULL);
            if (client_sd < 0){
                perror("Error: Accept");
                return -1;
            } else {
                fds.push_back(client_sd);
            }
        }

        for (int i = 0; i < (int)fds.size(); ++i){
            if (FD_ISSET(fds[i], &readfds)){

                char buffer;
                string request;
                bool closed = false;

                int current_server = fds[i];
                browser_ip = getIp(fds[i]);

                while (true){
                    int byte_recv = recv(current_server, &buffer, 1, 0);
                    if (byte_recv < 0){
                        close(server_socket);
                        return -1;
                    }
                    else if (byte_recv == 0){
                        perror("Connection closed");
                        if (fds.size() != 0)
                            fds.erase(fds.begin() + i);
                        if (throughput.find(current_server) != throughput.end())
                            throughput.erase(current_server);
                        closed = true;
                        break;
                    }
                    request += buffer;
                    if (request.size() >= 4){
                        if ((request.substr(request.size() - 4), "\r\n\r\n") == 0) 
                            break;
                    }
                }

                struct timeval t1, t2;

                if (!closed){

                    int current_bitrate;
                    string chunkName;

                    if (checkVideoData(request)){

                        chunkName = getChunkName(request);
                        int seg_pos = chunkName.find("Seg");
                        int bitrate = atoi(chunkName.substr(0, seg_pos).c_str());
                        current_bitrate = bitrate;

                        double input = throughput[current_server];
                        current_bitrate = newBitrate(input, bitrates);

                        if (current_bitrate != bitrate){
                            string newChunkName = to_string(current_bitrate) + chunkName.substr(seg_pos);
                            request = getResponseWithNewChunkName(request, newChunkName);
                            chunkName = newChunkName;
                        }
                    }

                    int server_sd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    if (server_sd < 0)
                    {
                        perror("Error creating socket");
                        close(server_socket);
                        return -1;
                    }

                    struct hostent *host = gethostbyname(www_ip);

                    struct sockaddr_in address;
                    address.sin_family = AF_INET;
                    address.sin_addr.s_addr = *(unsigned long *)host->h_addr_list[0];
                    address.sin_port = htons(80);

                    int success_connect = connect(server_sd, (const struct sockaddr *)&address, sizeof(address));
                    if (success_connect < 0){
                        perror("Error connection");
                        close(server_socket);
                        close(server_sd);
                        return -1;
                    }

                    if (send(server_sd, request.c_str(), request.size(), 0) < 0)
                    {
                        perror("Error sending to server");
                        close(server_socket);
                        close(server_sd);
                        return -1;
                    }

                    string response = recvResponse(server_sd);
                    string contentType = getValue(response, "Content-Type");
                    int content_len = atoi(getValue(response, "Content-Length").c_str());

                    gettimeofday(&t1, NULL);

                    for (int i = 0; i < content_len; i++)
                    {
                        if (recv(server_sd, &buffer, 1, 0) < 0)
                        {
                            perror("Error recv from server");
                            close(server_socket);
                            close(server_sd);
                            return -1;
                        }
                        response += buffer;
                    }

                    gettimeofday(&t2, NULL);

                    if (contentType.find("text/xml") != -1){
                        int index = 0;
                        int current = response.find("bitrate=", index);
                        while (current != -1)
                        {
                            int brate_pos_start = response.find('\"', current);
                            int brate_pos_end = response.find('\"', brate_pos_start + 1);
                            int bitrate = atoi(response.substr(brate_pos_start + 1, brate_pos_end - brate_pos_start - 1).c_str());
                            
                            bool isNew = true;
                            for (int bit : bitrates)
                            {
                                if (bitrate == bit){
                                    isNew = false;
                                    break;
                                }
                                isNew = true;
                            }

                            if (isNew){
                                bitrates.push_back(bitrate);
                            }

                            std::sort(bitrates.begin(), bitrates.end());
                            index = brate_pos_end;
                            current = response.find("bitrate=", index);
                        }

                        string request_chunk = request;
                        request_chunk.replace(request.find(".f4m"), 4, "_nolist.f4m");

                        if (send(server_sd, request_chunk.c_str(), request_chunk.size(), 0) < 0){
                            perror("Error sending to server");
                            close(server_socket);
                            close(server_sd);
                            return -1;
                        }

                        response = recvResponse(server_sd);
                        content_len = atoi(getValue(response, "Content-Length").c_str());
                        for (int i = 0; i < content_len; i++)
                        {
                            if (recv(server_sd, &buffer, 1, 0) < 0)
                            {
                                perror("Error: recv from server");
                                close(server_socket);
                                close(server_sd);
                                return -1;
                            }
                            response += buffer;
                        }

                    } else if (contentType.find("video/f4f") != -1){

                        double duration = totalTime(t1,t2);
                        double tput = getThroughput(content_len, duration);
                        double current_tput;

                        if (throughput.find(current_server) == throughput.end())
                            current_tput = bitrates.front();
                        else
                            current_tput = throughput[current_server];

                        current_tput = alpha * tput + (1 - alpha) * current_tput;
                        throughput[current_server] = current_tput;

                        logfile << browser_ip << " " << chunkName << " " << string(www_ip) << " " << duration << " " << tput << " " << current_tput << " " << current_bitrate << endl;
                        logfile.flush();
                    }

                    if (send(current_server, response.c_str(), response.size(), 0) < 0){
                        perror("Error: Send to client");
                        close(server_socket);
                        close(server_sd);
                        return -1;
                    }
                }
            }
        }
    }

    logfile.close();
    close(server_socket);
    return 0;
}
