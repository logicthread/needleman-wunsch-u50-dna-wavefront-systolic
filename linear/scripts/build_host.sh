set -e
# S1t gate host: S1c layout + on-card traceback (kernel_tile_1b_s1t.cpp)
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
O=${OUT_DIR:-$ROOT/build/host}; mkdir -p "$O"; cd "$O"
source /opt/xilinx/xrt/setup.sh >/dev/null
H=$ROOT/linear/host; S=$ROOT/linear/src
gcc -std=c99 -O2 -I$S -I$H -c $S/nw_ref.c -o nw_ref_c.o
gcc -std=c99 -O2 -DNWH_TILE_PE=128 -DNWH_DIR_B3 -DNWH_CONT -DNWH_BWIN=640 -I$S -I$H -c $H/nw_host_traceback.c -o tb3c.o
g++ -std=c++17 -O2 -Wall -Wextra -DNW_PE=${PE:-3968} -DWV_TILE_PE=128 -DWV_IFACE=2 -DWV_DIR_B3 -DNWH_DIR_B3 -DNWH_CONT -DNWH_BWIN=640 -DWV_ONCARD_TB \
    -I$XILINX_XRT/include -I$S -I$H -c $H/nw_gate_host.cpp -o gh3t.o
g++ gh3t.o tb3c.o nw_ref_c.o -o host_${PE:-3968} -L$XILINX_XRT/lib -lxrt_coreutil -lpthread -Wl,--allow-shlib-undefined
rm -f gh3t.o tb3c.o nw_ref_c.o; ls -la host_${PE:-3968}; md5sum host_${PE:-3968}
