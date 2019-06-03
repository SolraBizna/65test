#!/bin/sh

set -e

if ! find tmp -name \*.test -size +0c -exec false {} +; then
    echo "*** The following tests are failing: ***"
    find tmp -name \*.test -size +0c | sort | sed -E -e 's/^/	/'
    echo "Cat a file for more information on the failure."
    exit 1
else
    echo "All tests pass!"
fi
