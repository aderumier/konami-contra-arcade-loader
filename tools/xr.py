import pefile,capstone,sys,re
pe=pefile.PE(sys.argv[1]); base=pe.OPTIONAL_HEADER.ImageBase
img=pe.get_memory_mapped_image()
text=[s for s in pe.sections if s.Name.startswith(b'.text')][0]
tva=base+text.VirtualAddress; tdata=img[text.VirtualAddress:text.VirtualAddress+text.Misc_VirtualSize]
iat={}
for e in pe.DIRECTORY_ENTRY_IMPORT:
    for i in e.imports: iat[i.address]=(i.name or b'#%d'%i.ordinal).decode()
def s(va):
    o=va-base
    if 0<=o<len(img):
        b=img[o:o+80].split(b'\0')[0]
        if len(b)>=3 and all(32<=c<127 for c in b): return b.decode()
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
want=sys.argv[2]
ins=list(md.disasm(tdata,tva))
for k,i in enumerate(ins):
    m=re.search(r'0x([0-9a-f]+)',i.op_str)
    if m and iat.get(int(m.group(1),16))==want or (i.mnemonic=='call' and want.startswith('0x') and i.op_str==want):
        print('---',hex(i.address))
        for j in ins[max(0,k-int(sys.argv[3] if len(sys.argv)>3 else 10)):k+2]:
            ann=''
            for mm in re.findall(r'0x[0-9a-f]+',j.op_str):
                v=int(mm,16)
                if s(v): ann+=' "%s"'%s(v)
                if v in iat: ann+=' ['+iat[v]+']'
            print('  %x: %s %s%s'%(j.address,j.mnemonic,j.op_str,ann))
