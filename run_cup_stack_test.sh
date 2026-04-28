#!/bin/bash

# Cup Stack Test launcher: ensures USB perms, compiles, then runs.
# Pass-through args go to the binary, e.g.
#   ./run_cup_stack_test.sh                # 100/100/3000ms (defaults)
#   ./run_cup_stack_test.sh 100 100 4000   # 100/100/4000ms

if [ -e /dev/ttyACM0 ]; then
    if [ ! -r /dev/ttyACM0 ] || [ ! -w /dev/ttyACM0 ]; then
        echo "Setting USB permissions on /dev/ttyACM0..."
        sudo chmod 666 /dev/ttyACM0
    fi
fi

chmod +x compile_cup_stack_test.sh
./compile_cup_stack_test.sh

if [ $? -eq 0 ]; then
    ./cup_stack_test "$@"
fi
