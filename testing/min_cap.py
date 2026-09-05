import socket, time

N = 5
socks = [socket.create_connection(("127.0.0.1", 8080)) for _ in range(N)]
time.sleep(0.5)  # let all connections settle/register in epoll first

for s in socks:
    s.sendall(f"LOGIN user{id(s)}\n".encode())

time.sleep(1)
for s in socks:
    print(s.recv(1024))
