#!/usr/bin/env python3
"""Check original Frond IR probe logs and an independently generated binary payload."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile


def database_records(raw):
    if len(raw)<78 or struct.unpack_from('>H',raw,32)[0]&1:
        raise ValueError('expected a record database')
    count=struct.unpack_from('>H',raw,76)[0]
    end=78+8*count
    if end>len(raw):raise ValueError('truncated record table')
    offsets=[struct.unpack_from('>I',raw,78+8*i)[0] for i in range(count)]
    if any(n<end or n>len(raw) for n in offsets) or offsets!=sorted(set(offsets)):
        raise ValueError('invalid record offsets')
    return [raw[a:b] for a,b in zip(offsets,offsets[1:]+[len(raw)])]


def read_database(path,name,required=True):
    with zipfile.ZipFile(path) as archive:
        matches=[]
        for entry in archive.infolist():
            if entry.is_dir() or entry.file_size<78:continue
            # Exported names are incidental; identify the actual Palm database header.
            with archive.open(entry) as source:header=source.read(32)
            if header.rstrip(b'\0')==name.encode():matches.append(entry)
        if not matches and not required:return None
        if len(matches)!=1:raise ValueError('need exactly one '+name+' database')
        if matches[0].file_size>1024*1024:raise ValueError('probe database is unexpectedly large')
        return archive.read(matches[0])


def read_records(path,name):
    return database_records(read_database(path,name))


def fresh_logs(path,name,before=None):
    current=read_records(path,name)
    if before:
        prior=read_records(before,name)
        if len(current)<=len(prior) or current[:len(prior)]!=prior:
            raise ValueError('log did not append a fresh unchanged-history invocation')
        current=current[len(prior):]
    return [row.rstrip(b'\0').decode('ascii') for row in current]


def check_database(sender,receiver,sender_before=None,receiver_before=None):
    tx=fresh_logs(sender,'FrondDBBeamLog',sender_before)
    rx=fresh_logs(receiver,'FrondDBBeamLog',receiver_before)
    identity=b'Frond 5f original record database beam\nTX and E2 native Exchange Manager\n\0'
    payload=bytes(((i*73+17)^((i>>8)*31))&255 for i in range(4096))
    expected=[identity,payload]
    # Palm's writer emits the 78-byte header, two 8-byte entries and 2-byte pad.
    size=78+16+2+sum(map(len,expected))
    if tx[-5:]!=['create err=0000 bytes=0','put err=0000 bytes=0',
                 f'write err=0000 bytes={size}',f'disconnect err=0000 bytes={size}',f'complete err=0000 bytes={size}']:
        raise ValueError('sender has no complete successful database transfer')
    if rx[-4:]!=['accept err=0000 bytes=0',f'read err=0000 bytes={size}',
                 f'receive-disconnect err=0000 bytes={size}',f'receive-complete err=0000 bytes={size}']:
        raise ValueError('receiver has no complete successful database import')
    databases=[read_database(path,'Frond5FRecord') for path in (sender,receiver)]
    for raw in databases:
        if raw[60:68]!=b'DATAFrRD' or database_records(raw)!=expected:
            raise ValueError('database identity, record count or record bytes differ')
    # Importing updates the modification date; preserve all other metadata,
    # including record IDs/attributes, as well as the exact record contents.
    normalize=lambda raw:raw[:40]+bytes(4)+raw[44:]
    if normalize(databases[0])!=normalize(databases[1]):
        raise ValueError('imported database metadata or record identities differ')
    for before in (sender_before,receiver_before):
        if before and read_database(before,'Frond5FRecord',False) is not None:
            raise ValueError('test database existed before the transfer')
    return {'status':'pass','scope':'original ExgDBWrite/ExgDBRead completion and exact two-record database import',
            'serialized_bytes':size,'record_lengths':list(map(len,expected)),
            'record_sha256':[hashlib.sha256(row).hexdigest() for row in expected]}


def check(sender,receiver,sender_before=None,receiver_before=None):
    tx=read_records(sender,'FrondIRLog');rx=read_records(receiver,'FrondIRLog')
    for current,before,label in [(tx,sender_before,'sender'),(rx,receiver_before,'receiver')]:
        if before:
            prior=read_records(before,'FrondIRLog')
            if len(current)<=len(prior) or current[:len(prior)]!=prior:
                raise ValueError(label+' log did not append a fresh unchanged-history invocation')
            del current[:len(prior)]
    tx=[s.rstrip(b'\0').decode('ascii') for s in tx];rx=[s.rstrip(b'\0').decode('ascii') for s in rx]
    tx_expected=['send-begin','put err=0000 bytes=0','send err=0000 bytes=4096',
                 'disconnect err=0000 bytes=4096','send-complete']
    rx_expected=['receive-begin','accept err=0000 bytes=0','receive err=0000 bytes=4096','receive-complete']
    if tx[-5:]!=tx_expected:raise ValueError('sender has no complete successful transfer')
    if rx[-4:]!=rx_expected:raise ValueError('receiver has no complete successful transfer')
    payload=read_records(receiver,'FrondIRReceived')
    expected=bytes(((i*73+17)^((i>>8)*31))&255 for i in range(4096))
    if payload!=[expected]:raise ValueError('received payload differs in length, record count or bytes')
    return {'status':'pass','scope':'guest Exchange Manager completion and exact4096-byte delivery',
            'received_bytes':4096,'payload_sha256':hashlib.sha256(expected).hexdigest()}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('sender',type=Path);p.add_argument('receiver',type=Path)
    p.add_argument('--sender-before',type=Path);p.add_argument('--receiver-before',type=Path)
    p.add_argument('--database',action='store_true',help='check the irda-db probe and original Palm database importer')
    a=p.parse_args()
    try:result=(check_database if a.database else check)(a.sender,a.receiver,a.sender_before,a.receiver_before)
    except (ValueError,KeyError,OSError,UnicodeError,struct.error,zipfile.BadZipFile) as error:
        p.exit(1,str(error)+'\n')
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
