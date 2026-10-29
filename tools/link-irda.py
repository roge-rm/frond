#!/usr/bin/env python3
"""Link two Frond loopback TCP infrared endpoints without interpreting guest frames."""
import argparse
import json
import selectors
import socket
import time

QUEUE_BYTES=4096


def link(first,second,seconds=None):
    peers=[];selector=selectors.DefaultSelector();pending=[bytearray(),bytearray()]
    received=[0,0];sent=[0,0];started=time.monotonic();reason='deadline'
    try:
        for port in (first,second):
            peer=socket.create_connection(('127.0.0.1',port),timeout=10)
            peer.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1);peer.setblocking(False);peers.append(peer)
        print('Infrared endpoints linked on 127.0.0.1.',flush=True)
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
        for peer in peers:peer.close()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('first',type=int,help='TCP port from the first Frond --irda tcp:PORT endpoint')
    p.add_argument('second',type=int,help='TCP port from the second Frond --irda tcp:PORT endpoint')
    p.add_argument('--seconds',type=float,help='optional bounded run')
    a=p.parse_args()
    if not all(1<=x<=65535 for x in (a.first,a.second)):p.error('ports must be1..65535')
    if a.first==a.second:p.error('choose two different endpoints')
    if a.seconds is not None and not 0<a.seconds<=86400:p.error('seconds must be positive and at most86400')
    try:print(json.dumps(link(a.first,a.second,a.seconds)))
    except KeyboardInterrupt:return 0
    except OSError as e:p.exit(1,str(e)+'\n')
    return 0
if __name__=='__main__':raise SystemExit(main())
