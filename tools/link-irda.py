#!/usr/bin/env python3
"""Link two Frond infrared endpoints without interpreting guest frames.

Each endpoint is PORT (a local Frond's --irda tcp:PORT), HOST:PORT (connect to another
machine, such as an Android Frond waiting for a peer) or listen:PORT (wait on all
interfaces for one peer, such as an Android Frond connecting here). A local Frond whose
Palm sleeps closes its port until it wakes; PORT is connected again meanwhile."""
import argparse
import json
import selectors
import socket
import time

QUEUE_BYTES=4096


def endpoint(text):
    """Returns ('local','127.0.0.1',port), ('connect',host,port) or ('listen','',port)."""
    text=str(text).strip()
    if text.startswith('listen:'):kind,host,port='listen','',text[7:]
    elif text.startswith('['):
        host,sep,port=text[1:].partition(']:')
        if not sep or not host:raise ValueError(text)
        kind='connect'
    elif ':' in text:
        kind='connect';host,port=text.rsplit(':',1)
        if not host or ':' in host:raise ValueError(text)
    else:kind,host,port='local','127.0.0.1',text
    if not port.isdigit() or not 1<=int(port)<=65535:raise ValueError(text)
    return kind,host,int(port)


def name(e):
    kind,host,port=e
    if kind=='local':return str(port)
    return f'listen:{port}' if kind=='listen' else (f'[{host}]:{port}' if ':' in host else f'{host}:{port}')


RETRY_SECONDS=.3


def link(first,second,seconds=None):
    """Joins the endpoints until one ends. A local core's port (PORT) that closes, as a sleeping Palm's does
    until it wakes, is connected again for as long as the other side stays up; bytes for it meanwhile are
    dropped, as IrDA retries. A network peer closing, or both sides down at once, ends the run."""
    peers=[None,None];listeners={};selector=selectors.DefaultSelector();pending=[bytearray(),bytearray()]
    received=[0,0];sent=[0,0];dropped=[0,0];reconnects=[0,0];retry=[0.0,0.0];started=time.monotonic();reason='deadline'
    ends=[endpoint(e) for e in (first,second)]
    def result():return {'reason':reason,'received':received,'sent':sent,'dropped':dropped,'reconnects':reconnects}
    def ready(i,peer):
        peer.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1);peer.setblocking(False);peers[i]=peer
    def closed(i):
        # True when the run ends: a network peer, or both sides at once. A local core is connected again.
        nonlocal reason
        kind,_,port=ends[i]
        if kind!='local' or peers[1-i] is None:
            reason='peer disconnected'
            return True
        down(i);retry[i]=time.monotonic()+RETRY_SECONDS
        print(f'Core on port {port} closed; connecting again.',flush=True)
        return False
    def down(i):
        try:selector.unregister(peers[i])
        except (KeyError,ValueError):pass
        peers[i].close();peers[i]=None;pending[i].clear()
    try:
        # Listeners first, so a peer can connect while the other side is being reached.
        for i,(kind,_,port) in enumerate(ends):
            if kind=='listen':
                dual=socket.has_dualstack_ipv6()
                server=socket.create_server(('',port),family=socket.AF_INET6 if dual else socket.AF_INET,backlog=1,dualstack_ipv6=dual)
                listeners[i]=server
                print(f'Waiting for a peer on port {port}.',flush=True)
        def left():return None if seconds is None else seconds-(time.monotonic()-started)
        for i,(kind,host,port) in enumerate(ends):
            # A bounded run also ends while waiting for a peer or connecting.
            if left() is not None and left()<=0:return result()
            if kind=='listen':
                listeners[i].settimeout(left())
                try:peer,address=listeners[i].accept()
                except socket.timeout:return result()
                listeners.pop(i).close()
                print(f'Peer {address[0].removeprefix("::ffff:")} connected on port {port}.',flush=True)
            else:peer=socket.create_connection((host,port),timeout=10 if left() is None else max(.01,min(10,left())))
            ready(i,peer)
        print(f'Infrared endpoints linked: {name(ends[0])} and {name(ends[1])}.',flush=True)
        while seconds is None or time.monotonic()-started<seconds:
            for i,(kind,host,port) in enumerate(ends):
                if peers[i] is not None or time.monotonic()<retry[i]:continue
                try:peer=socket.create_connection((host,port),timeout=1)
                except OSError:retry[i]=time.monotonic()+RETRY_SECONDS;continue
                ready(i,peer);reconnects[i]+=1
                print(f'Core on port {port} is back.',flush=True)
            for i,peer in enumerate(peers):
                if peer is None:continue
                events=(selectors.EVENT_READ if len(pending[1-i])<QUEUE_BYTES else 0)
                if pending[i]:events|=selectors.EVENT_WRITE
                try:selector.get_key(peer)
                except KeyError:
                    if events:selector.register(peer,events,i)
                else:
                    if events:selector.modify(peer,events,i)
                    else:selector.unregister(peer)
            timeout=.1 if None in peers else .5
            if seconds is not None:timeout=max(0,min(timeout,seconds-(time.monotonic()-started)))
            if not selector.get_map():
                time.sleep(timeout);continue
            for key,events in selector.select(timeout):
                i=key.data;peer=key.fileobj
                if peers[i] is not peer:continue
                if events&selectors.EVENT_READ:
                    try:data=peer.recv(QUEUE_BYTES-len(pending[1-i]))
                    except BlockingIOError:data=None
                    except OSError:data=b''
                    if data==b'':
                        if closed(i):return result()
                        continue
                    if data:
                        received[i]+=len(data)
                        if peers[1-i] is None:dropped[1-i]+=len(data)
                        else:pending[1-i].extend(data)
                if events&selectors.EVENT_WRITE and pending[i]:
                    try:count=peer.send(pending[i])
                    except BlockingIOError:continue
                    # A side that can't take bytes has gone, the same as one that closed.
                    except OSError:
                        if closed(i):return result()
                        continue
                    del pending[i][:count];sent[i]+=count
        return result()
    finally:
        selector.close()
        for peer in [p for p in peers if p is not None]+list(listeners.values()):peer.close()


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
