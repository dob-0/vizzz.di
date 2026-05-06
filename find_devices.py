#!/usr/bin/env python3
"""Find vizzz.di nodes on the local network.

Usage:
    python3 find_devices.py
    python3 find_devices.py 192.168.1   # explicit subnet prefix
"""
import sys, socket, json, concurrent.futures, urllib.request

def probe(ip):
    try:
        with urllib.request.urlopen(f'http://{ip}/discover', timeout=0.8) as r:
            d = json.loads(r.read())
            if d.get('product') == 'vizzz.di':
                return ip, d
    except:
        pass
    return None

def local_prefix():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1)
    try:
        s.connect(('192.168.0.1', 1))
        ip = s.getsockname()[0]
    except:
        ip = '127.0.0.1'
    finally:
        s.close()
    return '.'.join(ip.split('.')[:3])

prefix = sys.argv[1] if len(sys.argv) > 1 else local_prefix()
print(f'Scanning {prefix}.0/24 for vizzz.di nodes...')

found = []
candidates = [f'{prefix}.{i}' for i in range(1, 255)]
with concurrent.futures.ThreadPoolExecutor(max_workers=50) as ex:
    for r in ex.map(probe, candidates):
        if r:
            found.append(r)

if found:
    for ip, d in found:
        print(f"  {d['name']:<20} http://{ip:<18} AP: http://{d['ap_ip']}  mDNS: {d['mdns']}")
else:
    print('  No devices found.')
    print(f'  Tip: connect to the device AP (vizzz.di_XXXXXX) and open http://10.0.0.1')
