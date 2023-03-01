#!/bin/bash
# type "finish" to exit

make clean

make

./ram_access

bash plot.sh
