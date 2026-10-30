#!/usr/bin/env python3
"""Actual loopback TCP relay: both directions, bounded-queue backpressure and EOF."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import json
import socket
import subprocess
import sys
import time
import unittest
class Tests(unittest.TestCase):
    def test_binary_duplex_and_disconnect(self):
        listeners=[];peers=[];process=None
        try:
            for _ in range(2):
                s=socket.socket();s.bind(('127.0.0.1',0));s.listen();s.settimeout(10);listeners.append(s)
            process=subprocess.Popen([sys.executable,str(Path(__file__).resolve().parents[1]/'tools/link-irda.py'),*[str(s.getsockname()[1]) for s in listeners]],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            for s in listeners:p,_=s.accept();p.settimeout(10);peers.append(p)
            self.assertIn('linked',process.stdout.readline())
            first=bytes(range(256))*256;second=bytes(reversed(range(256)))*257
            def receive(peer,size):
                data=bytearray()
                while len(data)<size:
                    chunk=peer.recv(min(333,size-len(data)));self.assertTrue(chunk);data.extend(chunk)
                return bytes(data)
            with ThreadPoolExecutor(max_workers=4) as pool:
                a=pool.submit(peers[0].sendall,first);b=pool.submit(peers[1].sendall,second)
                x=pool.submit(receive,peers[0],len(second));y=pool.submit(receive,peers[1],len(first))
                a.result();b.result();self.assertEqual(x.result(),second);self.assertEqual(y.result(),first)
            # Both local cores gone at once ends the run (one alone is connected again).
            for s in listeners:s.close()
            for p in peers:p.shutdown(socket.SHUT_RDWR);p.close()
            peers.clear()
            out,err=process.communicate(timeout=10);self.assertEqual(process.returncode,0,err)
            result=json.loads(out.splitlines()[-1]);self.assertEqual(result['received'],[len(first),len(second)])
            self.assertEqual(result['sent'],[len(second),len(first)]);self.assertEqual(result['reason'],'peer disconnected')
        finally:
            for p in peers+listeners:p.close()
            if process and process.poll() is None:process.kill();process.wait()
    def test_lan_endpoints(self):
        # listen:PORT takes a peer from the network, HOST:PORT connects out; plain ports stay 127.0.0.1.
        core=socket.socket();core.bind(('127.0.0.1',0));core.listen();core.settimeout(10)
        free=socket.socket();free.bind(('',0));port=free.getsockname()[1];free.close()
        process=None;peers=[]
        try:
            process=subprocess.Popen([sys.executable,str(Path(__file__).resolve().parents[1]/'tools/link-irda.py'),f'listen:{port}',f'127.0.0.1:{core.getsockname()[1]}'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            self.assertIn('Waiting',process.stdout.readline())
            remote=socket.create_connection(('127.0.0.1',port),timeout=10);peers.append(remote)
            local,_=core.accept();local.settimeout(10);peers.append(local)
            self.assertIn('connected',process.stdout.readline());self.assertIn('linked',process.stdout.readline())
            remote.sendall(b'beam');self.assertEqual(local.recv(4),b'beam')
            local.sendall(b'back');self.assertEqual(remote.recv(4),b'back')
            remote.close();peers.remove(remote)
            out,err=process.communicate(timeout=10);self.assertEqual(process.returncode,0,err)
            self.assertEqual(json.loads(out)['reason'],'peer disconnected')
        finally:
            for p in peers+[core]:p.close()
            if process and process.poll() is None:process.kill();process.wait()
    def test_local_core_reconnects(self):
        # A desktop Palm sleeping closes its core's port until it wakes; the network peer stays.
        core=socket.socket();core.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);core.bind(('127.0.0.1',0));core.listen();core.settimeout(10);port=core.getsockname()[1]
        free=socket.socket();free.bind(('',0));net=free.getsockname()[1];free.close()
        process=None;peers=[]
        try:
            process=subprocess.Popen([sys.executable,str(Path(__file__).resolve().parents[1]/'tools/link-irda.py'),str(port),f'listen:{net}','--seconds','20'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            self.assertIn('Waiting',process.stdout.readline())
            local,_=core.accept();local.settimeout(10);peers.append(local)
            remote=socket.create_connection(('127.0.0.1',net),timeout=10);peers.append(remote)
            self.assertIn('connected',process.stdout.readline());self.assertIn('linked',process.stdout.readline())
            remote.sendall(b'one');self.assertEqual(local.recv(3),b'one')
            # Sleep: the core closes its connection and its port.
            local.close();peers.remove(local);core.close()
            self.assertIn('closed',process.stdout.readline())
            remote.sendall(b'lost');time.sleep(.8)
            # Wake: the port opens again on the same number and the link connects to it.
            core=socket.socket();core.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);core.bind(('127.0.0.1',port));core.listen();core.settimeout(10)
            local,_=core.accept();local.settimeout(10);peers.append(local)
            self.assertIn('back',process.stdout.readline())
            remote.sendall(b'two');self.assertEqual(local.recv(3),b'two')
            local.sendall(b'back');self.assertEqual(remote.recv(4),b'back')
            remote.close();peers.remove(remote)
            out,err=process.communicate(timeout=10);self.assertEqual(process.returncode,0,err)
            result=json.loads(out.splitlines()[-1])
            self.assertEqual(result['reason'],'peer disconnected');self.assertEqual(result['reconnects'],[1,0])
            self.assertEqual(result['dropped'][0],4);self.assertEqual(result['received'],[4,10])
        finally:
            for p in peers+[core]:p.close()
            if process and process.poll() is None:process.kill();process.wait()
    def test_reconnect_deadline(self):
        # --seconds bounds a run whose local core never comes back.
        core=socket.socket();core.bind(('127.0.0.1',0));core.listen();core.settimeout(10);port=core.getsockname()[1]
        remote=socket.socket();remote.bind(('127.0.0.1',0));remote.listen();remote.settimeout(10)
        process=None;peers=[]
        try:
            started=time.monotonic()
            process=subprocess.Popen([sys.executable,str(Path(__file__).resolve().parents[1]/'tools/link-irda.py'),str(port),f'127.0.0.1:{remote.getsockname()[1]}','--seconds','2'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            local,_=core.accept();peers.append(local);other,_=remote.accept();peers.append(other)
            self.assertIn('linked',process.stdout.readline())
            local.close();peers.remove(local);core.close()
            out,err=process.communicate(timeout=10);self.assertEqual(process.returncode,0,err)
            self.assertLess(time.monotonic()-started,6)
            self.assertEqual(json.loads(out.splitlines()[-1])['reason'],'deadline')
        finally:
            for p in peers+[core,remote]:p.close()
            if process and process.poll() is None:process.kill();process.wait()
    def test_listen_deadline(self):
        # --seconds also ends a run still waiting for its listen: peer.
        free=socket.socket();free.bind(('',0));port=free.getsockname()[1];free.close()
        started=time.monotonic()
        p=subprocess.run([sys.executable,str(Path(__file__).resolve().parents[1]/'tools/link-irda.py'),f'listen:{port}','1234','--seconds','0.5'],capture_output=True,text=True,timeout=10)
        self.assertEqual(p.returncode,0,p.stderr);self.assertLess(time.monotonic()-started,5)
        self.assertEqual(json.loads(p.stdout.splitlines()[-1])['reason'],'deadline')
    def test_bad_endpoint_refused(self):
        for bad in ('listen:0','host:','0','listen:x','a:b:c'):
            p=subprocess.run([sys.executable,str(Path(__file__).resolve().parents[1]/'tools/link-irda.py'),bad,'1234'],capture_output=True,text=True)
            self.assertNotEqual(p.returncode,0,bad);self.assertIn('HOST:PORT',p.stderr)
    def test_same_endpoint_refused(self):
        p=subprocess.run([sys.executable,str(Path(__file__).resolve().parents[1]/'tools/link-irda.py'),'1234','1234'],capture_output=True,text=True)
        self.assertNotEqual(p.returncode,0);self.assertIn('different',p.stderr)
if __name__=='__main__':unittest.main()
