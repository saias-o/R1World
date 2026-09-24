#!/bin/sh
# Build the headless tools: r1cook (cook tiles from the cache) and r1test (the
# generator's tests). Objects are rebuilt only when out of date, in parallel.
set -e
cd "$(dirname "$0")"
export PATH="/c/msys64/ucrt64/bin:$PATH"
OUT=../generated/tools
mkdir -p $OUT/obj
FLAGS="-std=gnu++17 -O2 -ffp-contract=off -Wall -Wextra -I. -Ithird_party/clipper2/include -Ithird_party/earcut -I../../engine/third_party/json"
for f in gen/*.cpp third_party/clipper2/src/*.cpp tools/r1cook.cpp tests/*.cpp; do
  o=$OUT/obj/$(basename $(dirname $f))_$(basename $f .cpp).o
  if [ ! -f $o ] || [ $f -nt $o ] || [ -n "$(find gen tests -name '*.hpp' -newer $o)" ]; then
    g++ $FLAGS -c $f -o $o &
  fi
done
wait
LIB=$(ls $OUT/obj/gen_*.o $OUT/obj/src_*.o)
g++ $LIB $OUT/obj/tools_r1cook.o -o $OUT/r1cook.exe -lwinhttp -static-libgcc -static-libstdc++
g++ $LIB $OUT/obj/tests_*.o -o $OUT/r1test.exe -lwinhttp -static-libgcc -static-libstdc++
