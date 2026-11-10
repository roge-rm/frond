import socket
class Rsp:
 def __init__(self,port):
  self.s=socket.create_connection(('127.0.0.1',port));self.s.settimeout(30);self.s.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
 def put(self,cmd):
  b=cmd.encode();self.s.sendall(b'$'+b+b'#'+f'{sum(b)&255:02x}'.encode())
 def get(self):
  while True:
   byte=self.s.recv(1)
   if not byte:raise EOFError('production debugger disconnected')
   if byte==b'$':break
  out=b''
  while True:
   b=self.s.recv(1)
   if not b:raise EOFError('truncated debugger packet')
   if b==b'#':break
   out+=b
  checksum=self.s.recv(2);assert int(checksum,16)==sum(out)&255
  self.s.sendall(b'+');return out.decode()
 def cmd(self,cmd):self.put(cmd);return self.get()
