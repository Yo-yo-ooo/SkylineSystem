#!/bin/bash
grep 'ARTLOG' /tmp/fc8.out | grep 'tree=0x413320' | \
sed -e "s/\[ARTLOG\] op=//" -e "s/ tree=0x413320 //" -e "s/ INS key='//" -e "s/ DEL key='//" -e "s/' len=.*//" \
> /tmp/art_ops.txt
echo "=== count: $(wc -l < /tmp/art_ops.txt) ==="
cat /tmp/art_ops.txt
