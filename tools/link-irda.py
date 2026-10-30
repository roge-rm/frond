#!/usr/bin/env python3
"""Link two Frond infrared endpoints without interpreting guest frames.

Each endpoint is PORT (a local Frond's --irda tcp:PORT), HOST:PORT (connect to another
machine, such as an Android Frond waiting for a peer) or listen:PORT (wait on all
interfaces for one peer, such as an Android Frond connecting here)."""
import argparse
import json
import selectors
import socket
import time

QUEUE_BYTES=4096


def endpoint(text):
    """Returns ('connect',host,port) or ('listen','',port); a plain port is 127.0.0.1."""
    text=str(text).strip()
    if text.startswith('listen:'):kind,host,port='listen','',text[7:]
    elif text.startswith('['):
        host,sep,port=text[1:].partition(']:')
        if not sep or not host:raise ValueError(text)
        kind='connect'
    elif ':' in text:
        kind='connect';host,port=text.rsplit(':',1)
        if not host or ':' in host:raise ValueError(text)
    else:kind,host,port='connect','127.0.0.1',text
    if not port.isdigit() or not 1<=int(port)<=65535:raise ValueError(text)
    return kind,host,int(port)


def name(e):
    kind,host,port=e
    return f'listen:{port}' if kind=='listen' else (f'[{host}]:{port}' if ':' in host else f'{host}:{port}')


def link(first,second,seconds=None):
    peers=[];listeners={};selector=selectors.DefaultSelector();pending=[bytearray(),bytearray()]
    received=[0,0];sent=[0,0];started=time.monotonic();reason='deadline'
    ends=[endpoint(e) for e in (first,second)]
    try:
        # Listeners first, so a peer can connect while the other side is being reached.
        for i,(kind,_,port) in enumerate(ends):
            if kind=='listen':
                dual=socket.has_dualstack_ipv6()
                server=socket.create_server(('',port),family=socket.AF_INET6 if dual else socket.AF_INET,backlog=1,dualstack_ipv6=dual)
                listeners[i]=server
                print(f'Waiting for a peer on port {port}.',flush=True)
        for i,(kind,host,port) in enumerate(ends):
            if kind=='listen':
                peer,address=listeners[i].accept();listeners.pop(i).close()
                print(f'Peer {address[0].removeprefix("::ffff:")} connected on port {port}.',flush=True)
            else:peer=socket.create_connection((host,port),timeout=10)
            peer.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1);peer.setblocking(False);peers.append(peer)
        print(f'Infrared endpoints linked: {name(ends[0])} and {name(ends[1])}.',flush=True)
        while seconds is None or time.monotonic()-started<seconds:
            for i,peer in enumerate(peers):
                events=(selectors.EVENT_READ if len(pending[1-i])<QUEUE_BYTES else 0)
                if pending[i]:events|=selectors.EVENT_WRITE
                try:selector.get_key(peer)
                except KeyError:
                    if events:selector.register(peer,events,i)
                else:
                    if events:selector.modify(peer,events,i)
                    else:selector.unregister(peer)
            timeout=.5 if seconds is None else max(0,min(.5,seconds-(time.monotonic()-started)))
            for key,events in selector.select(timeout):
                i=key.data;peer=key.fileobj
                if events&selectors.EVENT_READ:
                    try:data=peer.recv(QUEUE_BYTES-len(pending[1-i]))
                    except BlockingIOError:continue
                    if not data:
                        reason='peer disconnected'
                        return {'reason':reason,'received':received,'sent':sent}
                    pending[1-i].extend(data);received[i]+=len(data)
                if events&selectors.EVENT_WRITE:
                    try:count=peer.send(pending[i])
                    except BlockingIOError:continue
                    del pending[i][:count];sent[i]+=count
        return {'reason':reason,'received':received,'sent':sent}
    finally:
        selector.close()
        for peer in peers+list(listeners.values()):peer.close()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('first',help='PORT, HOST:PORT or listen:PORT')
    p.add_argument('second',help='PORT, HOST:PORT or listen:PORT')
    p.add_argument('--seconds',type=float,help='optional bounded run')
    a=p.parse_args()
    try:ends=[endpoint(a.first),endpoint(a.second)]
    except ValueError as e:p.error(f'{e}: use PORT, HOST:PORT or listen:PORT with a port 1..65535')
    if ends[0]==ends[1]:p.error('choose two different endpoints')
    if a.seconds is not None and not 0<a.seconds<=86400:p.error('seconds must be positive and at most 86400')
    try:print(json.dumps(link(a.first,a.second,a.seconds)))
    except KeyboardInterrupt:return 0
    except OSError as e:p.exit(1,str(e)+'\n')
    return 0
if __name__=='__main__':raise SystemExit(main())
