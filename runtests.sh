#!/bin/bash
#
#
#
# *** If you edit this file, make sure that the instructions
# *** in README stay up to date.
#
#
#

export LANG=C  # "Segmentation fault" must be in english for this script to work
set -e -o pipefail

function usage() {
    echo "Usage: $0 [--valgrind] [--verbose] [--dont-run-make] [TEMPLATE]" >&2
    echo "TEMPLATE can be e.g. './jou %s', where %s will be replaced by a jou file." >&2
    exit 2
}

valgrind=no
verbose=no
run_make=yes

while [[ "$1" =~ ^- ]]; do
    case "$1" in
        --valgrind) valgrind=yes; shift ;;
        --verbose) verbose=yes; shift ;;
        --dont-run-make) run_make=no; shift ;;
        *) usage ;;
    esac
done

if [ $# == 0 ]; then
    # No arguments --> run tests in the basic/simple way
    if [[ "$OS" =~ Windows ]]; then
        command_template='./jou.exe %s'
    else
        command_template='./jou %s'
    fi
elif [ $# == 1 ]; then
    command_template="$1"
else
    usage
fi

if [ $valgrind = yes ]; then
    command_template="valgrind -q --leak-check=full --show-leak-kinds=all --suppressions=valgrind-suppressions.sup $command_template"
fi

if [ $run_make = yes ]; then
    if [[ "$OS" =~ Windows ]]; then
        source activate
        mingw32-make
    else
        make
    fi
fi

rm -rf tmp/tests
mkdir -p tmp/tests

joudir="$(pwd)"
if [[ "$OS" =~ Windows ]]; then
    jouexe="jou.exe"
    if [[ "$joudir" =~ ^/[A-Za-z]/ ]]; then
        # Rewrite funny mingw path: /d/a/jou/jou --> D:/a/jou/jou
        letter=${joudir:1:1}
        joudir="${letter^^}:/${joudir:3}"
        unset letter
    fi
else
    jouexe="./jou"
fi
echo "<joudir> in expected output will be replaced with $joudir."
echo "<jouexe> in expected output will be replaced with $jouexe."

function generate_expected_output()
{
    local joufile="$1"
    local correct_exit_code="$2"

    # In verbose mode, the output is silenced, see below. The point of
    # testing with --verbose is that the compiler shouldn't crash (#65).
    if [[ "$command_template" =~ --verbose ]]; then
        echo "A lot of output hidden..."
    else
        (
            (grep -onH '# Warning: .*' $joufile || true) | sed -E s/'(.*):([0-9]*):# Warning: '/'compiler warning for file "\1", line \2: '/
            (grep -onH '# Error: .*' $joufile || true) | sed -E s/'(.*):([0-9]*):# Error: '/'compiler error in file "\1", line \2: '/
            (grep -oE '# Output:.*' $joufile || true) | sed -E s/'^# Output: ?'//
        ) | sed "s,<joudir>,$joudir,g" | sed "s,<jouexe>,$jouexe,g"
    fi
    echo "Exit code: $correct_exit_code"
}

function post_process_output()
{
    local joufile="$1"
    if [[ "$command_template" =~ --verbose ]]; then
        # There is a LOT of output. We don't want to write the expected
        # output precisely somewhere, that would be a lot of work.
        # Instead, ignore the output and check only the exit code.
        echo "A lot of output hidden..."
        grep "^Exit code:"
    elif [[ $joufile =~ ^tests/crash/ ]]; then
        if [[ "$OS" =~ Windows ]]; then
            # Windows doesn't say "Segmentation fault" when a program crashes
            echo "Segmentation fault"
        fi
        # Hide most of the output. We really only care about whether it
        # mentions "Segmentation fault" somewhere inside it.
        grep -oE "Segmentation fault|Exit code: .*"
    else
        # Pass the output through unchanged.
        cat
    fi
}

YELLOW="\x1b[33m"
GREEN="\x1b[32m"
RED="\x1b[31m"
RESET="\x1b[0m"

if [ $verbose = yes ]; then
    function show_run() { echo "run: $1"; }
    function show_skip() { echo -e "${YELLOW}skip${RESET} $1"; }
    function show_ok() { echo -e "${GREEN}ok${RESET}   $1"; }
    function show_fail() { echo -e "${RED}FAIL${RESET} $1"; }
else
    function show_run() { true; }
    function show_skip() { echo -ne ${YELLOW}s${RESET}; }
    function show_ok() { echo -ne ${GREEN}.${RESET}; }
    function show_fail() { echo -ne ${RED}F${RESET}; }
fi

function run_test()
{
    local joufile="$1"
    local correct_exit_code="$2"
    local counter="$3"

    local command="$(printf "$command_template" $joufile)"
    local diffpath=tmp/tests/diff$(printf "%04d" $counter).txt  # consistent alphabetical order
    printf "\n\n\x1b[33m*** Command: %s ***\x1b[0m\n\n" "$command" > $diffpath

    # Skip tests when:
    #   * the test is supposed to crash, but optimizations are enabled (unpredictable by design)
    #   * the test is supposed to fail (crash or otherwise) and we use valgrind (see README)
    #   * the "test" is actually a GUI program in examples/
    if ( ! [[ "$command_template" =~ -O0 ]] && [[ $joufile =~ ^tests/crash/ ]] ) \
        || ( [[ "$command_template" =~ valgrind ]] && [ $correct_exit_code != 0 ] ) \
        || [ $joufile = examples/x11_window.jou ]
    then
        show_skip $joufile
        mv $diffpath $diffpath.skip
        return
    fi

    show_run $joufile
    if diff --text -u --color=always \
        <(generate_expected_output $joufile $correct_exit_code | tr -d '\r') \
        <(ulimit -v 500000 2>/dev/null; bash -c "$command; echo Exit code: \$?" 2>&1 | post_process_output $joufile | tr -d '\r') \
        &>> $diffpath
    then
        show_ok $joufile
        rm -f $diffpath
    else
        show_fail $joufile
        # Do not delete diff file. It will be displayed at end of test run.
    fi
}

counter=0
for joufile in examples/*.jou tests/*/*.jou; do
    case $joufile in
        examples/* | tests/should_succeed/*) correct_exit_code=0; ;;
        *) correct_exit_code=1; ;;  # compiler or runtime error
    esac
    counter=$((counter + 1))

    # Run 2 tests in parallel.
    while [ $(jobs -p | wc -l) -ge 2 ]; do wait -n; done
    run_test $joufile $correct_exit_code $counter &
done
wait

echo ""
echo ""

failed=$( (ls -1 tmp/tests/diff*.txt 2>/dev/null || true) | wc -l)
skipped=$( (ls -1 tmp/tests/diff*.txt.skip 2>/dev/null || true) | wc -l)
succeeded=$((counter - failed - skipped))

if [ $failed != 0 ]; then
    echo "------- FAILURES -------"
    cat tmp/tests/diff*.txt
fi

echo -ne "${GREEN}${succeeded} succeeded"
if [ $failed != 0 ]; then
    echo -ne ", ${RED}${failed} failed"
fi
if [ $skipped != 0 ]; then
    echo -ne ", ${YELLOW}${skipped} skipped"
fi
echo -e $RESET

exit $((!!failed))
