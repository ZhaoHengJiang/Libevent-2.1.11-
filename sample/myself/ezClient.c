#include <stdio.h>  
    #include <stdlib.h>  
    #include <string.h>  
      
    #include <sys/types.h>    
    #include <sys/socket.h>    
    #include <netinet/in.h>    
    #include <arpa/inet.h>   
    #include <pthread.h>    
      
    int main() {  
          
        int client_fd; //定义一个客户端的SOCKET  
      
        struct sockaddr_in server_addr; //服务器端  
        memset(&server_addr,0,sizeof(server_addr)); //数据初始化--清零    
        server_addr.sin_family=AF_INET; //设置为IP通信    
        server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");//服务器IP地址    
        server_addr.sin_port = htons(8001); //服务器端口号    
      
        client_fd = socket(PF_INET, SOCK_STREAM, 0);  
        if (client_fd < 1) {  
            puts("client socket error");  
            return 0;  
        }  
      
        /*将套接字绑定到服务器的网络地址上,并且连接服务器端*/    
        int ret = connect(client_fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr));  
        if (ret < 0) {  
            puts("client connect error!");  
            return 0;  
        }  
      
        char buf[1024];  
        int len = recv(client_fd, buf, 1024, 0); //等待接收服务器端的数据  
        buf[len] = '\0';  
        puts(buf);  
      
        char *x = "Hello World,saodsadoosadosa==sadsad==";  
        send(client_fd, x, strlen(x), 0); //发送数据  
      
        memset(buf, 0, 1024);  
        int len2 = recv(client_fd, buf, 1024, 0); //继续接收服务端返回的数据  
        buf[len2] = '\0';  
        puts(buf);  
      
        shutdown(client_fd,2); //关闭socket  
      
    }
