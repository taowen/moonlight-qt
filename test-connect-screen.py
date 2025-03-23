import socket
import time
import sys

def test_connect_server(connect_screen_req):
    """
    测试 ConnectScreenServer 的客户端脚本
    
    参数:
        connect_screen_req: 要发送给服务器的 request
    """
    server_ip="127.0.0.1"
    server_port=42515
    try:
        # 创建 TCP 套接字
        client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        print(f"正在连接到服务器 {server_ip}:{server_port}...")
        
        # 连接到服务器
        client_socket.connect((server_ip, server_port))
        print("已成功连接到服务器")
        
        # 发送 IP 地址给服务器
        message = f"{connect_screen_req}\n"
        client_socket.sendall(message.encode('utf-8'))
        print(f"已发送 IP 地址: {connect_screen_req}")
        
        # 接收服务器响应
        response = client_socket.recv(1024).decode('utf-8').strip()
        print(f"服务器响应: {response}")
        
        # 保持连接一段时间
        print("保持连接 5 秒...")
        time.sleep(5)
        
        # 关闭连接
        client_socket.close()
        print("已关闭连接")
        
        return True
        
    except ConnectionRefusedError:
        print(f"错误: 无法连接到服务器 {server_ip}:{server_port}，请确认服务器正在运行")
        return False
    except Exception as e:
        print(f"发生错误: {str(e)}")
        return False

if __name__ == "__main__":
    test_connect_server('{"ip":"127.0.0.1","uuid":"65A72D07-AD95-C820-9603-350BE33FE695","pin":"1234"}')