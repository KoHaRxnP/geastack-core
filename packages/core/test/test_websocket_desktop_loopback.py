#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Authored loopback RFC6455 peer; never connects to an existing application."""
import base64, hashlib, socket, struct, subprocess, sys, threading, time

def exact(c,n):
    b=b''
    while len(b)<n:
        part=c.recv(n-len(b))
        if not part: raise RuntimeError('unexpected EOF')
        b+=part
    return b

def frame(c,opcode,payload,fin=True):
    n=len(payload)
    c.sendall(bytes([(128 if fin else 0)|opcode, n if n<126 else 126 if n<65536 else 127])+(struct.pack('!H',n) if 126<=n<65536 else struct.pack('!Q',n) if n>=65536 else b'')+payload)

def read(c):
    a,b=exact(c,2); assert b&128, 'client frame must be masked'
    n=b&127
    if n==126:n=struct.unpack('!H',exact(c,2))[0]
    if n==127:n=struct.unpack('!Q',exact(c,8))[0]
    assert n<=1024*1024
    mask=exact(c,4); data=exact(c,n)
    return a&15, bytes(v^mask[i%4] for i,v in enumerate(data))

errors=[]
with socket.socket() as server:
    if len(sys.argv)>2 and sys.argv[2]=='--partial':server.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,16384)
    server.bind(('127.0.0.1',0)); server.listen(); server.settimeout(10)
    def peer():
        try:
            with server.accept()[0] as c:
                c.settimeout(8); header=b''
                while not header.endswith(b'\r\n\r\n'):
                    header+=exact(c,1); assert len(header)<16384
                lines=header.decode().split('\r\n'); fields=dict(line.split(':',1) for line in lines[1:] if ':' in line)
                fields={k.lower():v.strip() for k,v in fields.items()}
                assert fields['sec-websocket-protocol']=='gea-test' and fields['x-gea-test']=='loopback'
                accept=base64.b64encode(hashlib.sha1((fields['sec-websocket-key']+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
                c.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+b'\r\nSec-WebSocket-Protocol: gea-test\r\n\r\n')
                assert read(c)==(1,b'hello')
                frame(c,1,b'fragmented ',False); frame(c,9,b'ping'); frame(c,0,b'text')
                frame(c,2,b'ignored binary'); frame(c,1,b'sink'); frame(c,1,b'x'*70000); frame(c,1,b'')
                assert read(c)==(10,b'ping')
                frame(c,8,struct.pack('!H',1000)+b'done')
                assert read(c)[0]==8
            # Second connection exercises native producer and local close.
            with server.accept()[0] as c:
                c.settimeout(8); header=b''
                while not header.endswith(b'\r\n\r\n'):header+=exact(c,1)
                fields={k.lower():v.strip() for k,v in (line.split(':',1) for line in header.decode().split('\r\n')[1:] if ':' in line)}
                accept=base64.b64encode(hashlib.sha1((fields['sec-websocket-key']+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
                c.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+b'\r\n\r\n')
                assert read(c)==(1,b'produced');frame(c,1,b'ready')
                while True:
                    opcode,data=read(c)
                    if opcode==8:frame(c,8,data);break
                    assert (opcode,data)==(1,b'produced')
            with server.accept()[0] as c:
                c.settimeout(8);header=b''
                while not header.endswith(b'\r\n\r\n'):header+=exact(c,1)
                c.sendall(b'HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n')
            with server.accept()[0] as c:
                c.settimeout(8);header=b''
                while not header.endswith(b'\r\n\r\n'):header+=exact(c,1)
                fields={k.lower():v.strip() for k,v in (line.split(':',1) for line in header.decode().split('\r\n')[1:] if ':' in line)}
                accept=base64.b64encode(hashlib.sha1((fields['sec-websocket-key']+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
                c.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+b'\r\n\r\n')
                assert read(c)==(1,b'closing-produced');frame(c,1,b'ready')
                while True:
                    opcode,data=read(c)
                    if opcode==8:frame(c,8,data);break
                    assert (opcode,data)==(1,b'closing-produced')
            if len(sys.argv)>2 and sys.argv[2]=='--partial':
                with server.accept()[0] as c:
                    c.settimeout(8);header=b''
                    while not header.endswith(b'\r\n\r\n'):header+=exact(c,1)
                    fields={k.lower():v.strip() for k,v in (line.split(':',1) for line in header.decode().split('\r\n')[1:] if ':' in line)}
                    accept=base64.b64encode(hashlib.sha1((fields['sec-websocket-key']+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
                    c.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+b'\r\n\r\n')
                    time.sleep(0.3) # Receiver window + test-only small client send buffer.
                    assert read(c)==(1,b'p'*500000)
                    opcode,payload=read(c);assert opcode==8
                    frame(c,8,payload)
        except BaseException as e:errors.append(e)
    t=threading.Thread(target=peer);t.start()
    result=subprocess.run([sys.argv[1],f'ws://127.0.0.1:{server.getsockname()[1]}/test'],timeout=20)
    t.join(10); assert not t.is_alive(); assert not errors,errors
    assert result.returncode==0,result.returncode
