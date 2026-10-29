#!/usr/bin/env python3
"""Negative controls for actual IR payload and fresh-invocation acceptance."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile
spec=importlib.util.spec_from_file_location('checker',Path(__file__).resolve().parents[1]/'tools/check-irda-probe.py')
c=importlib.util.module_from_spec(spec);spec.loader.exec_module(c)
def pdb(name,records):
    raw=bytearray(80+8*len(records));raw[:len(name)]=name.encode();struct.pack_into('>H',raw,76,len(records))
    for i,record in enumerate(records):struct.pack_into('>I',raw,78+8*i,len(raw));raw.extend(record)
    return raw
class Tests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)
        self.tx=[b'register err=0000 bytes=0',b'send-begin',b'put err=0000 bytes=0',b'send err=0000 bytes=4096',b'disconnect err=0000 bytes=4096',b'send-complete']
        self.rx=[b'register err=0000 bytes=0',b'receive-begin',b'accept err=0000 bytes=0',b'receive err=0000 bytes=4096',b'receive-complete']
        self.payload=bytes(((i*73+17)^((i>>8)*31))&255 for i in range(4096))
    def tearDown(self):self.tmp.cleanup()
    def archive(self,path,log,payload=None):
        with zipfile.ZipFile(self.root/path,'w') as z:
            z.writestr('log.pdb',pdb('FrondIRLog',[x+b'\0' for x in log]))
            if payload is not None:z.writestr('data.pdb',pdb('FrondIRReceived',[payload]))
        return self.root/path
    def check(self,tx=None,rx=None,payload=None,**args):
        return c.check(self.archive('tx.zip',self.tx if tx is None else tx),self.archive('rx.zip',self.rx if rx is None else rx,self.payload if payload is None else payload),**args)
    def test_exact_delivery(self):self.assertEqual(self.check()['status'],'pass')
    def test_no_sender_completion(self):
        with self.assertRaises(ValueError):self.check(tx=self.tx[:-1])
    def test_no_receiver_completion(self):
        with self.assertRaises(ValueError):self.check(rx=self.rx[:-1])
    def test_api_error_cannot_pass(self):
        tx=self.tx.copy();tx[3]=b'send err=150c bytes=4096'
        with self.assertRaises(ValueError):self.check(tx=tx)
    def test_short_payload_cannot_pass(self):
        with self.assertRaises(ValueError):self.check(payload=self.payload[:-1])
    def test_corrupt_payload_cannot_pass(self):
        data=bytearray(self.payload);data[500]^=1
        with self.assertRaises(ValueError):self.check(payload=bytes(data))
    def test_stale_success_cannot_pass(self):
        before=self.archive('before.zip',self.rx,self.payload)
        with self.assertRaises(ValueError):self.check(receiver_before=before)
    def test_changed_history_cannot_pass(self):
        before=self.archive('before.zip',[b'other'],self.payload)
        with self.assertRaises(ValueError):self.check(receiver_before=before)
    def test_appended_success(self):
        before=self.archive('before.zip',self.rx[:1],self.payload)
        self.assertEqual(self.check(receiver_before=before)['status'],'pass')
    def test_bad_offsets(self):
        raw=pdb('FrondIRLog',[b'one',b'two']);struct.pack_into('>I',raw,78,2)
        with self.assertRaises(ValueError):c.database_records(raw)
    def test_duplicate_database(self):
        path=self.archive('dup.zip',self.rx,self.payload)
        with zipfile.ZipFile(path,'a') as z:z.writestr('extra.pdb',pdb('FrondIRLog',self.rx))
        with self.assertRaises(ValueError):c.read_records(path,'FrondIRLog')
class DatabaseTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)
        identity=b'Frond 5f original record database beam\nTX and E2 native Exchange Manager\n\0'
        payload=bytes(((i*73+17)^((i>>8)*31))&255 for i in range(4096))
        self.raw=pdb('Frond5FRecord',[identity,payload]);self.raw[60:68]=b'DATAFrRD'
        self.raw[83:86]=(1).to_bytes(3,'big');self.raw[91:94]=(2).to_bytes(3,'big')
        self.tx=[b'register err=0000 bytes=0',b'create err=0000 bytes=0',b'put err=0000 bytes=0',b'write err=0000 bytes=4266',b'disconnect err=0000 bytes=4266',b'complete err=0000 bytes=4266']
        self.rx=[b'register err=0000 bytes=0',b'accept err=0000 bytes=0',b'read err=0000 bytes=4266',b'receive-disconnect err=0000 bytes=4266',b'receive-complete err=0000 bytes=4266']
    def tearDown(self):self.tmp.cleanup()
    def archive(self,name,log,raw=None):
        path=self.root/name
        with zipfile.ZipFile(path,'w') as z:
            z.writestr('events.pdb',pdb('FrondDBBeamLog',[row+b'\0' for row in log]))
            if raw is not None:z.writestr('records.pdb',raw)
        return path
    def check(self,raw=None,tx=None,rx=None,**kwargs):
        return c.check_database(self.archive('tx.zip',self.tx if tx is None else tx,self.raw),
                                self.archive('rx.zip',self.rx if rx is None else rx,self.raw if raw is None else raw),**kwargs)
    def test_import_updates_only_modification_date(self):
        raw=self.raw.copy();struct.pack_into('>I',raw,40,123456)
        self.assertEqual(self.check(raw)['serialized_bytes'],4266)
    def test_record_corruption_rejected(self):
        raw=self.raw.copy();raw[-1]^=1
        with self.assertRaises(ValueError):self.check(raw)
    def test_record_identity_change_rejected(self):
        raw=self.raw.copy();raw[85]^=1
        with self.assertRaises(ValueError):self.check(raw)
    def test_wrong_database_creator_rejected(self):
        raw=self.raw.copy();raw[64:68]=b'BAD!'
        with self.assertRaises(ValueError):self.check(raw)
    def test_disconnect_error_rejected(self):
        tx=self.tx.copy();tx[-2]=b'disconnect err=1503 bytes=4266'
        with self.assertRaises(ValueError):self.check(tx=tx)
    def test_incomplete_original_import_rejected(self):
        rx=self.rx.copy();rx[2]=b'read err=0000 bytes=100'
        with self.assertRaises(ValueError):self.check(rx=rx)
    def test_existing_receiver_database_rejected(self):
        before=self.archive('before.zip',self.rx[:1],self.raw)
        with self.assertRaises(ValueError):self.check(receiver_before=before)
    def test_fresh_database_import_history(self):
        before=self.archive('before.zip',self.rx[:1])
        self.assertEqual(self.check(receiver_before=before)['status'],'pass')
    def test_stale_database_success_rejected(self):
        before=self.archive('before.zip',self.rx)
        with self.assertRaises(ValueError):self.check(receiver_before=before)

if __name__=='__main__':unittest.main()
