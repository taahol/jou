#!/bin/bash
set -e -o pipefail

# Go to project root.
cd "$(dirname "$0")"/..

rm -rf tests/tmp
mkdir tests/tmp
mkdir tests/tmp/diffs

succeeded=0
failed=0

function generate_expected_output()
{
    local filename="$1"
    (grep -o '# Output: .*' $joufile || true) | sed s/'^# Output: '//
    (grep -onH '# Error: .*' $joufile || true) | sed -E s/'(.*):([0-9]*):# Error: '/'compiler error in file "\1", line \2: '/
}

for joufile in examples/*.jou tests/*.jou; do
    command="./jou $joufile"
    if diff -u --color=always \
        <(generate_expected_output $joufile) \
        <(bash -c "$command" 2>&1) \
        &> tests/tmp/diffs/$(echo -n "$command" | base64)
    then
        echo -ne "\x1b[32m.\x1b[0m"
        succeeded=$((succeeded + 1))
    else
        echo -ne "\x1b[31mF\x1b[0m"
        failed=$((failed + 1))
    fi
done

echo ""
echo ""

if [ $failed != 0 ]; then
    echo "------- FAILURES -------"
    echo ""
    cd tests/tmp/diffs
    for b64filename in *; do
        echo "*** Command: $(echo $b64filename | base64 -d) ***"
        cat $b64filename
        echo ""
        echo ""
    done
fi

if [ $failed = 0 ]; then
    echo -e "\x1b[32m$succeeded succeeded\x1b[0m"
else
    echo -e "$succeeded succeeded, \x1b[31m$failed failed\x1b[0m"
    exit 1
fi
