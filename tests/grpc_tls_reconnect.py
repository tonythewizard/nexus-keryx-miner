import pathlib, subprocess, sys, time, shutil
client,mock,replay,root=sys.argv[1:]
root=pathlib.Path(root); root.mkdir(parents=True,exist_ok=True)
cert=root/'cert.pem'; key=root/'key.pem'; capture=root/'capture.bin'; log=root/'mock.log'
capture.unlink(missing_ok=True)
subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),'-out',str(cert),'-days','1','-subj','/CN=127.0.0.1','-addext','subjectAltName=IP:127.0.0.1'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
p=subprocess.Popen([mock,str(cert),str(key)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
try:
    line=p.stdout.readline().strip(); assert line.startswith('PORT=')
    endpoint='grpcs://127.0.0.1:'+line.split('=',1)[1]
    env=dict(__import__('os').environ); env['GRPC_DEFAULT_SSL_ROOTS_FILE_PATH']=str(cert)
    r=subprocess.run([client,'--dry-run','--endpoint',endpoint,'--pay-address','kaspa:mock','--extra-data','test','--capture',str(capture),'--max-templates','2','--reconnect-after-seconds','2'],text=True,capture_output=True,timeout=20,env=env)
    (root/'client.log').write_text(r.stdout+'\n'+r.stderr)
    assert r.returncode==0, r.stdout+r.stderr
    assert r.stdout.count('LIVE_TEMPLATE generation=')==2, r.stdout
    assert 'generation=2' in r.stdout and 'submit=DISABLED' in r.stdout
    q=subprocess.run([replay,str(capture)],text=True,capture_output=True,check=True)
    assert 'REPLAY_SUMMARY records=2 submit=DISABLED' in q.stdout
    bad=root/'tampered.bin'; shutil.copyfile(capture,bad)
    data=bytearray(bad.read_bytes()); data[-40]^=1; bad.write_bytes(data)
    reject=subprocess.run([replay,str(bad)],text=True,capture_output=True)
    assert reject.returncode!=0
    plain=subprocess.run([client,'--dry-run','--endpoint','grpc://example.com:22110','--pay-address','x','--extra-data','','--capture',str(root/'no.capture')],text=True,capture_output=True)
    assert plain.returncode!=0 and 'loopback' in plain.stderr
    submit=subprocess.run([client,'--dry-run','--endpoint','grpc://127.0.0.1:22110','--pay-address','x','--extra-data','','--capture',str(root/'no-submit.capture'),'--submit','true'],text=True,capture_output=True)
    assert submit.returncode!=0 and 'unsupported option: --submit' in submit.stderr
    print('GRPC_TLS_RECONNECT_REPLAY=PASS tamper=REJECTED plaintext_remote=REJECTED')
finally:
    try: p.stdin.write('\n'); p.stdin.flush()
    except Exception: pass
    p.wait(timeout=5)
