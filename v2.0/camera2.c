#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include <pthread.h>

void *receive_cmd(void *arg){
    int sock = *(int *)arg;
    char cmd[10];
     // 提前打开 LED 设备的文件描述符，不用每次都 open
    int led_fd = open("/sys/class/leds/sys-led/brightness", O_WRONLY);
    if (led_fd == -1) {
        perror("打开 LED 失败");
    }
    while(1){
        memset(cmd, 0, sizeof(cmd));
        int n = recv(sock, cmd, sizeof(cmd), 0);
        if (n > 0){
            if(led_fd != -1) {
                if(cmd[0] == '1'){
                    write(led_fd, "1", 1); // 极速亮灯！
                }else if (cmd[0] == '0'){
                    write(led_fd, "0", 1); // 极速灭灯！
                }
            }
        }else if (n <= 0){
            break;
        }
    }
    if (led_fd != -1) close(led_fd);
    return NULL;
}


// 很多摄像头在 640x480 下不支持 MJPG，但在 1080P 下支持。我们先试这个。
#define WIDTH 1920 
#define HEIGHT 1080
#define BUFFER_COUNT 4

int main() {
    int fd = open("/dev/video2", O_RDWR); // 依然打开 video2
    if (fd == -1) { perror("打开摄像头失败"); return -1; }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = WIDTH;
    fmt.fmt.pix.height = HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG; // 强行要 MJPG
    
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
        perror("设置 MJPG 格式失败"); 
        // 如果失败了，打印一下它实际把宽高改成了什么
        printf("实际得到的宽高: %d x %d\n", fmt.fmt.pix.width, fmt.fmt.pix.height);
        close(fd); return -1;
    }
    printf("格式设置成功！分辨率: %d x %d\n", fmt.fmt.pix.width, fmt.fmt.pix.height);

    // 申请缓冲区
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT; 
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ioctl(fd, VIDIOC_REQBUFS, &req);

    // 映射内存
    struct v4l2_buffer buf;
    void *buffers[BUFFER_COUNT];
    int i = 0;
    for(i = 0; i< BUFFER_COUNT; i++){
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        ioctl(fd, VIDIOC_QUERYBUF, &buf);

        buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        ioctl(fd, VIDIOC_QBUF, &buf);
    }

    
    // 入队并开启视频流
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMON, &type);

    // socket sending...
    int sock =socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "192.168.1.100", &server_addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1){
        perror("connect failed");
    }
    printf("connect successfully!");

    pthread_t pid;
    pthread_create(&pid, NULL, receive_cmd, &sock);
    pthread_detach(pid);

    int frame_count = 0;
    int fps = 0;
    time_t start = time(NULL);
    while(1){
        ioctl(fd, VIDIOC_DQBUF, &buf);

        // 4 bytes
        uint32_t net_len = htonl(buf.bytesused);
        send(sock, &net_len, sizeof(net_len), 0);

        send(sock, buffers[buf.index], buf.bytesused, 0);
        if(frame_count % 30 == 0) {
            printf("the %d image sent done! size: %d bytes\n", frame_count+1, buf.bytesused);
       }
        frame_count++;
        ioctl(fd, VIDIOC_QBUF, &buf);

        fps++;
        if (time(NULL) - start >= 1) {
            printf("当前实时帧率: %d 帧/秒\n", fps);
            fps = 0;
            start = time(NULL);
        }
    }


    // 清理
    close(sock);
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for( i=0; i<BUFFER_COUNT; i++) munmap(buffers[i], buf.length);   
    close(fd);
    return 0;
}