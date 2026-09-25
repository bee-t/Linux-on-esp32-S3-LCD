
import re

with open('C:/Users/abtcr/Documents/Arduino/libraries/Adafruit_GFX_Library/glcdfont.c', 'r') as f:
    data = f.read()

m = re.search(r'\{([\s\S]+?)\}', data)
hex_str = m.group(1)

# strip C++ comments
hex_str = re.sub(r'//.*', '', hex_str)
hex_str = re.sub(r'/\*.*?\*/', '', hex_str, flags=re.DOTALL)

hex_vals = [int(x.strip(), 16) for x in hex_str.split(',') if x.strip() != '']

print(f'Found {len(hex_vals)} bytes')

out = []
out.append('#pragma once')
out.append('const unsigned char font6x8[256][8] = {')
for i in range(256):
    cols = hex_vals[i*5 : i*5+5]
    out.append('    {')
    rows = []
    for y in range(8):
        row_val = 0
        for x in range(5):
            if cols[x] & (1 << y):
                row_val |= (1 << x)
        rows.append(f'0x{row_val:02x}')
    out.append('        ' + ', '.join(rows))
    out.append('    },')
out.append('};')

with open('paperboard/common/font6x8.h', 'w') as f:
    f.write('\n'.join(out))

