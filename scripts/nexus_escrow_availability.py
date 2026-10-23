#!/usr/bin/env python3
"""Read-only, cached UTXO availability. No signing, state mutation or CUDA initialization."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import threading
import time

ROOT = Path(os.environ.get("NEXUS_DATA_DIR", str(Path.home() / ".local/share/nexus-keryx-miner")))
PROBE = ROOT / 'updates-development/build/nexus-escrow-status-probe'
PROBE_SHA = '65477bcfde4784339aa3c9b74cf5e98a5f7221578b3b5ffe4c93eed7147032f3'
CACHE = ROOT / 'state/observability/escrow-available-v2.json'


def public_wallet():
    p = max((ROOT/'logs').glob('nexus-ui-inference-*.jsonl'), key=lambda p:p.stat().st_mtime_ns)
    with p.open('rb') as f:
        f.seek(max(0,p.stat().st_size-512*1024));lines=f.read().splitlines()[1:]
    for line in reversed(lines):
        try:
            record=json.loads(line);s=record.get('worker',record)
            if s.get('event')!='NEXUS_TELEMETRY':continue
            age=(time.monotonic_ns()-s['monotonic_ns'])/1e9
            w=s.get('wallet',{})
            if not 0<=age<=5 or w.get('authorization')!='AUTHORIZED' or w.get('verification')!='VALID':raise ValueError('wallet telemetry unavailable')
            key=w['escrow_public_key']
            if not isinstance(key,str) or len(key)!=64 or any(c not in '0123456789abcdef' for c in key):raise ValueError('public identity malformed')
            return key
        except json.JSONDecodeError:continue
    raise ValueError('wallet telemetry unavailable')


def snapshot_records():
    state=ROOT/'state/ai-service/escrow_state.json';journal=state.with_suffix('.journal')
    def signature(p):
        st=p.stat()
        if st.st_size>16*1024*1024 or p.is_symlink() or st.st_mode&0o077:raise ValueError('unsafe escrow state')
        return st.st_ino,st.st_size,st.st_mtime_ns
    before=(signature(state),signature(journal))
    obj=json.loads(state.read_text());entries=obj['entries'];seq=obj['journal_seq']
    if not isinstance(entries,list) or len(entries)>10000 or type(seq)is not int:raise ValueError('bounded escrow state invalid')
    index={}
    for e in entries:
        key=(e['coinbase_txid'],e['output_index'])
        previous=index.get(key)
        if previous is None:
            index[key]=dict(e)
            continue
        # Repeated stock discovery can persist the same outpoint more than once.
        # Never expose it as spendable if any copy marks it terminal. Conflicting
        # ownership, amount or maturity metadata still fails closed.
        fields=set(previous)|set(e)
        if any(previous.get(k)!=e.get(k) for k in fields-{'claimed','slashed'}):
            raise ValueError('conflicting escrow records')
        for flag in ('claimed','slashed'):
            if flag in fields:
                if type(previous.get(flag,False)) is not bool or type(e.get(flag,False)) is not bool:
                    raise ValueError('invalid escrow terminal state')
                previous[flag]=previous.get(flag,False) or e.get(flag,False)
    text=journal.read_text()
    if text and not text.endswith('\n'):raise ValueError('journal write in progress')
    for line in text.splitlines():
        if not line.strip():continue
        if len(line)>1048576:raise ValueError('oversized escrow record')
        row=json.loads(line);s,e=row['s'],row['e']
        if type(s)is not int:raise ValueError('invalid journal sequence')
        if s<=seq:continue
        if s!=seq+1:raise ValueError('journal sequence gap')
        seq=s;index[(e['coinbase_txid'],e['output_index'])]=e
    if before!=(signature(state),signature(journal)):raise ValueError('escrow state changed while reading')
    if len(index)>10000:raise ValueError('escrow record limit exceeded')
    return list(index.values())


class EscrowAvailabilityPoller:
    def __init__(self):self.next_poll=0;self.thread=None
    def pump(self):
        now=time.monotonic()
        if now>=self.next_poll and not(self.thread and self.thread.is_alive()):
            self.next_poll=now+60
            self.thread=threading.Thread(target=self.refresh,daemon=True,name='escrow-readonly-status');self.thread.start()
        try:
            d=json.loads(CACHE.read_text());age=time.time()-d.get('observed_unix',0)
            if d.get('status')!='AVAILABLE' or not 0<=age<=90:return {'status':'STALE' if d.get('status')=='AVAILABLE' else 'N/A','outputs':None,'amount_sompi':None}
            return {**d,'age_s':round(age,1)}
        except (OSError,ValueError):return {'status':'N/A','outputs':None,'amount_sompi':None}
    def refresh(self):
        CACHE.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
        fd=os.open(CACHE.parent/'escrow-available.lock',os.O_CREAT|os.O_RDWR|os.O_NOFOLLOW,0o600)
        try:
            fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
            try:
                d=json.loads(CACHE.read_text())
                if d.get('schema_version')==2 and 0<=time.time()-d.get('observed_unix',0)<60:return
            except (OSError,ValueError):pass
            try:
                if hashlib.sha256(PROBE.read_bytes()).hexdigest()!=PROBE_SHA:raise ValueError('read-only probe hash mismatch')
                key=public_wallet();lines=[key]
                for e in snapshot_records():
                    if e.get('claimed',False) or e.get('slashed',False):continue
                    txid=e['coinbase_txid']
                    if len(txid)!=64 or any(c not in '0123456789abcdef' for c in txid):raise ValueError('invalid escrow outpoint')
                    values=[e['output_index'],e['amount_sompi'],e.get('csv_window',36000),e['confirm_daa'],e.get('orphan_retry_after_daa')or 0,int(not e.get('is_inference',False)),e.get('batch_cap',0),int(e.get('orphan_slashed',False))]
                    if any(type(v)is not int or not 0<=v<=2**64-1 for v in values):raise ValueError('invalid escrow metadata')
                    lines.append(txid+' '+' '.join(map(str,values)))
                env=os.environ.copy();env.update(TMPDIR=str(ROOT/'escrow-utxo/tmp'),TMP=str(ROOT/'escrow-utxo/tmp'),TEMP=str(ROOT/'escrow-utxo/tmp'))
                r=subprocess.run([str(PROBE)],input='\n'.join(lines)+'\n',text=True,capture_output=True,timeout=25,env=env,check=True)
                data=json.loads(r.stdout);data['observed_unix']=time.time()
            except (OSError,ValueError,KeyError,TypeError,subprocess.SubprocessError):data={'status':'UNAVAILABLE','outputs':None,'amount_sompi':None,'observed_unix':time.time()}
            temp=CACHE.with_suffix('.new')
            out=os.open(temp,os.O_WRONLY|os.O_CREAT|os.O_TRUNC|os.O_NOFOLLOW,0o600)
            with os.fdopen(out,'w')as f:json.dump(data,f);f.flush();os.fsync(f.fileno())
            os.replace(temp,CACHE)
        except BlockingIOError:pass
        finally:os.close(fd)
