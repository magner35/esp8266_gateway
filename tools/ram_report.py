import re
BS = chr(92)
lines = open('.pio/build/esp8266_modern_rtos/esp8266_gateway_fork.map',
             encoding='utf-8', errors='replace').read().splitlines()
sec_re = re.compile(r'^(\.[\w.]+)\s+0x[0-9a-f]+\s+0x[0-9a-f]+')
ent_re = re.compile(r'^\s+(?:\.[\w.$]+\s+)?0x[0-9a-f]+\s+(0x[0-9a-f]+)\s+(\S+\.o)\s*$')
cur = None
agg = {}
syms = []
for l in lines:
    m = sec_re.match(l)
    if m:
        cur = m.group(1) if m.group(1).startswith('.dram0') else None
        continue
    m = ent_re.match(l)
    if m and cur in ('.dram0.data', '.dram0.bss'):
        size = int(m.group(1), 16)
        obj = m.group(2).replace(BS, '/')
        base = obj.split('/')[-1]
        agg[base] = agg.get(base, 0) + size
        cur_obj = base
tot = sum(agg.values())
print(f'parsed {tot} of 22888 static RAM (.data+.bss)')
ours = {}
for k, v in agg.items():
    if k in ('web.o', 'ske02.o', 'chartbuf.o', 'storage.o', 'protocol.o',
             'ntp.o', 'modbus_tcp.o', 'hex_parser.o', 'hex_store.o',
             'main.o', 'debug.o', 'wifi.o', 'dns.o', 'urclock.o',
             'tree_store.o', 'uart_avr.o', 'pages.o'):
        ours[k] = v
print('--- project .o (static data+bss) ---')
for k, v in sorted(ours.items(), key=lambda x: -x[1]):
    print(f'{v:6d}  {k}')
print(f'project total: {sum(ours.values())}')
print(f'SDK+libs: {tot - sum(ours.values())}')
