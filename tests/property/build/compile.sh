#!/bin/bash
src="$1"
obj="$(echo "$src" | sed 's|/home/cat/gsb/uid3957/src/||; s|/|_|g; s|\.cpp$|.o|')"
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -DUSE_ZLIB -I/home/cat/gsb/uid3957 -c "$src" -o "$obj" 2>> build_err2.log
