import pefile,capstone,sys,re
pe=pefile.PE(sys.argv[1]); base=pe.OPTIONAL_HEADER.ImageBase; img=pe.get_memory_mapped_image()
iat={}
for e in pe.DIRECTORY_ENTRY_IMPORT:
    for i in e.imports: iat[i.address]=(e.dll.decode().split('.')[0]+'!'+(i.name or b'#%d'%i.ordinal).decode())
def s(va):
    o=va-base
    if 0<=o<len(img):
        b=img[o:o+100].split(b'\0')[0]
        if len(b)>=3 and all(32<=c<127 or c in (9,10,13) for c in b): return b.decode()
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
a=int(sys.argv[2],16); n=int(sys.argv[3])
for j in md.disasm(img[a-base:a-base+n*8],a):
    if j.address>=a+n*4 and n>0: pass
    ann=''
    for mm in re.findall(r'0x[0-9a-f]+',j.op_str):
        v=int(mm,16)
        if s(v): ann+=' "%s"'%s(v)[:60]
        if v in iat: ann+=' ['+iat[v]+']'
    print('%x: %-8s %s%s'%(j.address,j.mnemonic,j.op_str,ann))
    if j.address>a+n*3: break
