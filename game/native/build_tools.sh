#!/bin/sh
# Build the headless tools: r1cook (cook tiles from the cache), r1relief (the
# planet's relief layer) and r1test (the generator's tests). Objects are
# rebuilt only when out of date, in parallel.
set -e
cd "$(dirname "$0")"
export PATH="/c/msys64/ucrt64/bin:$PATH"
OUT=${R1_TOOLS_OUT:-../generated/tools}
mkdir -p $OUT/obj $OUT/tmp
export TMPDIR="$(pwd)/$OUT/tmp" TEMP="$(pwd)/$OUT/tmp" TMP="$(pwd)/$OUT/tmp"
FLAGS="-std=gnu++17 -O2 -ffp-contract=off -Wall -Wextra -I. -Ithird_party/clipper2/include -Ithird_party/earcut -I../../engine/third_party/json"
PIDS=""
for f in minimap.cpp gen/*.cpp third_party/clipper2/src/*.cpp tools/r1cook.cpp tools/r1relief.cpp tests/*.cpp; do
  if [ "$f" = minimap.cpp ]; then o=$OUT/obj/minimap.o
  else o=$OUT/obj/$(basename $(dirname $f))_$(basename $f .cpp).o; fi
  if [ ! -f $o ] || [ $f -nt $o ] || [ -n "$(find gen tests . -maxdepth 1 -name '*.hpp' -newer $o)" ]; then
    g++ $FLAGS -c $f -o $o &
    PIDS="$PIDS $!"
  fi
done
FAILED=0
for pid in $PIDS; do
  if ! wait "$pid"; then FAILED=1; fi
done
if [ "$FAILED" != 0 ]; then exit 1; fi
LIB=$(ls $OUT/obj/gen_*.o $OUT/obj/src_*.o $OUT/obj/minimap.o)
g++ $LIB $OUT/obj/tools_r1cook.o -o $OUT/r1cook.exe -lwinhttp -static-libgcc -static-libstdc++
g++ $LIB $OUT/obj/tools_r1relief.o -o $OUT/r1relief.exe -lwinhttp -static-libgcc -static-libstdc++
g++ $LIB $OUT/obj/tests_*.o -o $OUT/r1test.exe -lwinhttp -static-libgcc -static-libstdc++
