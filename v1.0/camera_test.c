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

// 很多摄像头在 640x480 下不支持 MJPG，但在 1080P 下支持。我们先试这个。
#define WIDTH 1920 
#define HEIGHT 1080
#define FILENAME "test_v4l2.jpg"

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
    req.count = 1; 
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ioctl(fd, VIDIOC_REQBUFS, &req);

    // 映射内存
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    ioctl(fd, VIDIOC_QUERYBUF, &buf);

    void *buffer_start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
    
    // 入队并开启视频流
    ioctl(fd, VIDIOC_QBUF, &buf);
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMON, &type);

    // 等待数据
    ioctl(fd, VIDIOC_DQBUF, &buf);

    // 保存
    FILE *file = fopen(FILENAME, "wb");
    if (file) {
        fwrite(buffer_start, 1, buf.bytesused, file);
        fclose(file);
        printf("【成功抓拍 JPG】大小: %d 字节\n", buf.bytesused);
    }

    // socket sending...

    int sock =socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "192.168.1.100", &server_addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1){
        perror("connect failed");
    }else{
        printf("connected successfully! %d are sending...\n",buf.bytesused);
        send(sock, buffer_start, buf.bytesused, 0);
        printf("data sending successfully!\n");
        close(sock);
    }


    // 清理
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    munmap(buffer_start, buf.length);
    close(fd);
    return 0;
}