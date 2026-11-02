import socket
import struct
import cv2
import numpy as np
from ultralytics import YOLO
import time  
HOST = '0.0.0.0'
PORT = 8080

print("加载 YOLO 模型...")
model = YOLO('yolov8n.pt')

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind((HOST, PORT))
server.listen(5)

print(f"【云端】服务器已启动，正在监听 {PORT} 端口，等待板子连接...")
conn, addr = server.accept()
print(f"【云端】板子已连接: {addr[0]}")

frame_count = 0 
fps = 0.0             #  初始化 fps 变量
fps_start_time = time.time() #  记录开始计算帧率的时间

try:
    while True:
        # 1. 收齐 4 个字节的长度头
        header_data = b''
        while len(header_data) < 4:
            packet = conn.recv(4 - len(header_data))
            if not packet: break
            header_data += packet
        
        if len(header_data) < 4:
            print("板子断开了连接。")
            break

        # 2. 解析长度
        img_size = struct.unpack('>I', header_data)[0]
        # print(f"收到包头，即将接收 {img_size} 字节的图片...") # 刷屏太严重，建议注释掉

        # 3. 严格按照长度收满数据
        img_data = b''
        while len(img_data) < img_size:
            packet = conn.recv(img_size - len(img_data))
            if not packet: break
            img_data += packet

        # 4. 内存直接解码
        img_array = np.frombuffer(img_data, dtype=np.uint8)
        frame = cv2.imdecode(img_array, cv2.IMREAD_COLOR)

        if frame is not None:
            frame_count += 1
            
            # 5. 推理
            results = model(frame, verbose=False)
            annotated_frame = results[0].plot()
            
            # 6. 打印目标
            person_detected = False
            for r in results:
                for box in r.boxes:
                    cls_name = model.names[int(box.cls[0])]
                    if cls_name == 'person' and float(box.conf[0]) > 0.5:
                        person_detected = True
                        print(f" 第 {frame_count} 帧: 发现人！置信度 {float(box.conf[0]):.2f}")
            if person_detected:
                conn.send(b"1")
            else:
                conn.send(b"0")
             # 每处理 10 帧，计算一次平均帧率并更新
            if frame_count % 10 == 0:
                end_time = time.time()
                fps = 10 / (end_time - fps_start_time)
                fps_start_time = end_time
            # 7. 弹窗显示画面
            cv2.putText(annotated_frame, f"FPS: {fps:.2f}", (20, 50), 
            cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)
            cv2.imshow("EdgeVision - Real-time Video", annotated_frame)
            
            # 8. 刷新画面，按 q 退出
            if cv2.waitKey(1) & 0xFF == ord('q'):
                print("用户按下了 q ,准备退出...")
                break

except Exception as e:
    print(f"云端通信发生错误: {e}")
finally:
    conn.close()
    server.close()
    cv2.destroyAllWindows()
    print("程序已退出。")