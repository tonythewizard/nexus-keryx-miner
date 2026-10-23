import pathlib, subprocess, sys
protoc, current, old, out = sys.argv[1:]
out=pathlib.Path(out); out.mkdir(parents=True,exist_ok=True)
def compile_schema(schema,name):
    subprocess.run([protoc,f'-I{schema}',f'--descriptor_set_out={out/name}','rpc.proto','messages.proto'],check=True)
compile_schema(current,'current.pb'); compile_schema(old,'v057.pb')
def encode(schema,typ,payload,source='messages.proto'):
    return subprocess.run([protoc,f'-I{schema}',f'--encode={typ}',source],input=payload.encode(),capture_output=True,check=True).stdout
def decode(schema,typ,payload):
    return subprocess.run([protoc,f'-I{schema}',f'--decode={typ}','messages.proto'],input=payload,capture_output=True,check=True).stdout.decode()
req='getBlockTemplateRequest { payAddress: "address-sentinel" extraData: "extra-sentinel" }'
old_decoded=decode(old,'protowire.KaspadMessage',encode(current,'protowire.KaspadRequest',req))
assert 'getBlockTemplateRequest' in old_decoded and 'address-sentinel' in old_decoded and 'extra-sentinel' in old_decoded
resp='getBlockTemplateResponse { isSynced: true block { header { bits: 42 daaScore: 123 timestamp: 1700000000 } } }'
new_decoded=decode(current,'protowire.KaspadResponse',encode(old,'protowire.KaspadMessage',resp))
assert 'getBlockTemplateResponse' in new_decoded and 'daaScore: 123' in new_decoded and 'bits: 42' in new_decoded
print('PROTO_WIRE_COMPAT=PASS official_v057<->current request,response')
for typ,payload in [
    ('GetVirtualSelectedParentChainFromBlockRequestMessage', 'startHash: "a" includeAcceptedTransactionIds: true'),
    ('GetVirtualSelectedParentChainFromBlockResponseMessage', 'addedChainBlockHashes: "b" acceptedTransactionIds { acceptingBlockHash: "b" acceptedTransactionIds: "c" }'),
    ('NotifyVirtualSelectedParentChainChangedRequestMessage', 'includeAcceptedTransactionIds: true'),
    ('VirtualSelectedParentChainChangedNotificationMessage', 'addedChainBlockHashes: "b" acceptedTransactionIds { acceptingBlockHash: "b" acceptedTransactionIds: "c" }'),
]:
    current_typ=typ.replace('VirtualSelectedParentChain','VirtualChain')
    assert encode(current,'protowire.'+current_typ,payload,'rpc.proto') == encode(old,'protowire.'+typ,payload,'rpc.proto'), typ
print('SELECTED_CHAIN_ACCEPTANCE_WIRE=PASS restore,notification')
