#!/bin/zsh
# Quiet measurement pass: one benchmark at a time, gated on load and thermal state, machine state logged per block.
Q=${RAVEN_MAC_QUIET:?}
SP=${RAVEN_MAC_SPIKES:?}
OUT=$Q/results; mkdir -p $OUT/spikes/results $OUT/e $OUT/c $OUT/d
STATE=$OUT/state.csv
[ -f $STATE ] || echo "time,block,phase,load1,load5,thermal,power,battery" > $STATE
log_state() {
  local l=($(sysctl -n vm.loadavg | tr -d '{}'))
  local p=$(pmset -g batt | head -1 | sed -E "s/.*'(.*)'.*/\1/")
  local b=$(pmset -g batt | sed -n 2p | grep -oE '[0-9]+%')
  echo "$(date +%T),$1,$2,${l[1]},${l[2]},$($Q/thermal),$p,$b" >> $STATE
}
gate() {
  local i=0
  while :; do
    local l=$(sysctl -n vm.loadavg | awk '{print $2}')
    local t=$($Q/thermal)
    if (( $(echo "$l < 3.0" | bc) )) && [ "$t" -le 0 ]; then break; fi
    (( i++ )); if (( i > 40 )); then echo "gate timeout $1 load=$l thermal=$t"; break; fi
    sleep 15
  done
}
block() {
  local name=$1; shift
  sleep 60
  gate $name
  log_state $name start
  echo "=== $name $(date +%T)"
  "$@"
  echo "--- $name exit=$? $(date +%T)"
  log_state $name end
}
cd $OUT/spikes
# done in run 1: block spikes_gate $SP/gate
# done in run 1: block spikes_exc_signal zsh -c "for k in brk fault svc; do $SP/exc signal \$k; done"
# done in run 1: block spikes_exc_mach zsh -c "for k in brk fault; do $SP/exc mach \$k; $SP/exc mach \$k task; done"
# done in run 1: block spikes_hvf_roundtrip $SP/hvf roundtrip
# done in run 1: block spikes_hvf_shmspin $SP/hvf shmspin
block spikes_waits $SP/waits
block spikes_barriers $SP/barriers
block spikes_jit zsh -c "$SP/jit_adhoc_noruntime jit_adhoc_noruntime; $SP/jit_adhoc_runtime_allowjit jit_adhoc_runtime_allowjit"
block spikes_x18_toggle $SP/x18c cost
cd $Q/build-e3
block e_channel_native ./channel_test $OUT/e/channel-native.csv 20000
block e_boundary ./boundary_bench ./raven-guest-bootstrap ./fixtures/boundary-bench.exe $OUT/e/boundary.csv 20000 30
cd $Q
block c_translator python3 $Q/benchrepo/mac_bench.py $Q/build-translate $Q/harness $OUT/c 30
D=${RAVEN_BENCH_CHECKOUT:?}
W=${MCSODA:?}/bin
for g in call-x86_64 startup-x86_64 npp; do
  block d_wine_$g python3 $D/scripts/benchmark-macos.py --runtime wine --runner $W/wine --prefix ${WINEPREFIX_BASE:?} --work $Q/wine-work.noindex --out $OUT/d --sync msync --repeats 30 --warmup 3 --quiet-load 10 --quiet-wait 20 --timeout 300 --cc-dir ${LLVM_MINGW:?}/bin --npp ${NPP_X64:?} $g
done
echo "QUIET PASS DONE $(date +%T)"
