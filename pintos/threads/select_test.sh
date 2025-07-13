#!/usr/bin/env bash

# Usage: select_test.sh [-q|-g] [-r]
#   -q|-g : �ㅽ뻾 紐⑤뱶 吏���
#   -r    : clean & rebuild
if (( $# < 1 || $# > 2 )); then
  echo "Usage: $0 [-q|-g] [-r]"
  echo "  -q   : run tests quietly (no GDB stub)"
  echo "  -g   : attach via GDB stub (skip build)"
  echo "  -r   : force clean & full rebuild"
  exit 1
fi

MODE="$1"
if [[ "$MODE" != "-q" && "$MODE" != "-g" ]]; then
  echo "Usage: $0 [-q|-g] [-r]"
  exit 1
fi

REBUILD=0
if (( $# == 2 )); then
  if [[ "$2" == "-r" ]]; then
    REBUILD=1
  else
    echo "Unknown option: $2"
    echo "Usage: $0 [-q|-g] [-r]"
    exit 1
  fi
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../activate"

if [[ ! -d "${SCRIPT_DIR}/build" ]]; then
  echo "Build directory not found. Building Pintos threads..."
  make -C "${SCRIPT_DIR}" clean all
fi

if (( REBUILD )); then
  echo "Force rebuilding Pintos threads..."
  make -C "${SCRIPT_DIR}" clean all
fi

STATE_FILE="${SCRIPT_DIR}/.test_status"
declare -A status_map

if [[ -f "$STATE_FILE" ]]; then
  while read -r test stat; do
    status_map["$test"]="$stat"
  done < "$STATE_FILE"
fi

tests=(
  alarm-single
  alarm-multiple
  alarm-simultaneous
  alarm-priority
  alarm-zero
  alarm-negative
  priority-change
  priority-donate-one
  priority-donate-multiple
  priority-donate-multiple2
  priority-donate-nest
  priority-donate-sema
  priority-donate-lower
  priority-fifo
  priority-preempt
  priority-sema
  priority-condvar
  priority-donate-chain
  mlfqs-load-1
  mlfqs-load-60
  mlfqs-load-avg
  mlfqs-recent-1
  mlfqs-fair-2
  mlfqs-fair-20
  mlfqs-nice-2
  mlfqs-nice-10
  mlfqs-block
)

echo "=== Available Pintos Tests ==="
for i in "${!tests[@]}"; do
  idx=$((i+1))
  test="${tests[i]}"
  stat="${status_map[$test]:-untested}"
  case "$stat" in
    PASS) color="\e[32m" ;;
    FAIL) color="\e[31m" ;;
    *)    color="\e[0m"  ;;
  esac
  printf " ${color}%2d) %s\e[0m\n" "$idx" "$test"
done

read -p "Enter test numbers (e.g. '1 3 5' or '2-4'): " input
tokens=()
for tok in ${input//,/ }; do
  if [[ "$tok" =~ ^([0-9]+)-([0-9]+)$ ]]; then
    for ((n=${BASH_REMATCH[1]}; n<=${BASH_REMATCH[2]}; n++)); do
      tokens+=("$n")
    done
  else
    tokens+=("$tok")
  fi
done

declare -A seen=()
sel_tests=()
for n in "${tokens[@]}"; do
  if [[ "$n" =~ ^[0-9]+$ ]] && (( n>=1 && n<=${#tests[@]} )); then
    idx=$((n-1))
    if [[ -z "${seen[$idx]}" ]]; then
      sel_tests+=("${tests[idx]}")
      seen[$idx]=1
    fi
  else
    echo "Invalid test number: $n" >&2
    exit 1
  fi
done

echo "Selected tests: ${sel_tests[*]}"

passed=()
failed=()
{
  cd "${SCRIPT_DIR}/build" || exit 1

  count=0
  total=${#sel_tests[@]}
  for test in "${sel_tests[@]}"; do
    echo
    if [[ "$MODE" == "-q" ]]; then

      echo -n "Running ${test} in batch mode... "
      if make -s "tests/threads/${test}.result"; then
        if grep -q '^PASS' "tests/threads/${test}.result"; then
          echo "PASS"; passed+=("$test")
        else
          echo "FAIL"; failed+=("$test")
        fi
      else
        echo "ERROR"; failed+=("$test")
      fi
    else

      outdir="tests/threads"
      mkdir -p "${outdir}"

      echo -e "=== Debugging \e[33m${test}\e[0m ($(( count + 1 ))/${total}) ==="
      echo " * QEMU 李쎌씠 �④퀬, gdb stub�� localhost:1234 �먯꽌 ��湲고빀�덈떎."
      echo " * �대� 異쒕젰�� �곕��먯뿉 蹂댁씠硫댁꽌 '${outdir}/${test}.output'�먮룄 ���λ맗�덈떎."
      echo

      pintos --gdb -- -q run "${test}" 2>&1 | tee "${outdir}/${test}.output" 

      repo_root="${SCRIPT_DIR}/.."   
      ck="${repo_root}/tests/threads/${test}.ck"
      if [[ -f "$ck" ]]; then

      perl -I "${repo_root}" \
           "$ck" "${outdir}/${test}" "${outdir}/${test}.result"
        if grep -q '^PASS' "${outdir}/${test}.result"; then
          echo "=> PASS"; passed+=("$test")
        else
          echo "=> FAIL"; failed+=("$test")
        fi
      else
        echo "=> No .ck script, skipping result."; failed+=("$test")
      fi
      echo "=== ${test} session end ==="
    fi

    ((count++))
    echo -e "\e[33mtest ${count}/${total} finish\e[0m"
  done
}

echo
echo "=== Test Summary ==="
echo "Passed: ${#passed[@]}"
for t in "${passed[@]}"; do echo "  - $t"; done
echo "Failed: ${#failed[@]}"
for t in "${failed[@]}"; do echo "  - $t"; done

for t in "${passed[@]}"; do
  status_map["$t"]="PASS"
done

for t in "${failed[@]}"; do
  status_map["$t"]="FAIL"
done


> "$STATE_FILE"
for test in "${!status_map[@]}"; do
  echo "$test ${status_map[$test]}"
done >| "$STATE_FILE"