#!/bin/bash

# Count the number of FPGAs with device ID 10ee:903f
fpga_count=$(lspci -d 10ee:903f | wc -l)

echo "Number of FPGAs detected: $fpga_count"

if [ "$fpga_count" -eq 8 ]; then
    echo "Status: OK (8 FPGAs found)"
else
    echo "Status: WARNING (Expected 8, found $fpga_count)"
fi
