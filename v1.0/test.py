import socket
import os
from ultralytics import YOLO

# ================= 配置区 =================
HOST = '0.0.0.0'  # 监听所有本机网卡（接收来自 192.168.1.20 板子的连接）
PORT = 8080       # 监听端口，必须和板子 C 代码里填的一致
# ==========================================

def main():
    # ================= 第一部分：网络接收 =================
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_socket.bind((HOST, PORT))
    server_socket.listen(1)
    
    print(f"【云端】AI 服务器已启动，正在 {HOST}:{PORT} 等待板子连接...")
    print("👉 请立刻去板子上运行 ./camera_test")

    try:
        conn, addr = server_socket.accept()
        print(f"✅ 【云端】板子已连接！对方 IP: {addr[0]}")

        total_data = b''
        while True:
            data = conn.recv(4096)
            if not data:
                break
            total_data += data

        print(f"📦 【云端】图片接收完毕，共 {len(total_data)} 字节。")

        filename = "board_photo.jpg"
        with open(filename, "wb") as f:
            f.write(total_data)
        print(f"💾 【云端】图片已保存为 {filename}！")

    except Exception as e:
        print(f"❌ 【云端】发生错误: {e}")
        return # 如果网络接收失败，直接退出
    finally:
        if 'conn' in locals():
            conn.close()
        server_socket.close()

    # ================= 第二部分：AI 识别 =================
    print("\n🧠 【AI】正在加载 YOLOv8 模型...")
    # 第一次运行会自动下载 yolov8n.pt，之后就会直接加载
    model = YOLO('yolov8n.pt') 

    print("🔍 【AI】正在对接收到的图片进行识别...")
    results = model(filename)

    print("\n--- 🚨 识别结果 ---")
    # 遍历识别出的所有目标
    for r in results:
        for box in r.boxes:
            # 获取分类名字和置信度
            cls_name = model.names[int(box.cls[0])]
            conf = float(box.conf[0])
            print(f"🎯 发现目标: {cls_name}, 置信度: {conf:.2f}")

    # 保存画了框的结果图
    result_filename = "ai_result.jpg"
    results[0].save(result_filename)
    print(f"\n🎉 【完成】带识别框的图片已保存为 {result_filename}，快去双击打开看看！")

if __name__ == '__main__':
    main()