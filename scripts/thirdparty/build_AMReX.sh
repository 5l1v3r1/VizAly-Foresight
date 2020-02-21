#!/bin/bash

# SZ
echo "Building AMReX ... "

git clone https://github.com/AMReX-Codes/amrex
cd amrex

mkdir install
mkdir build
cd build

cmake .. -DCMAKE_INSTALL_PREFIX=../install
make -j
make install

cd ..

cd ..

echo "Building AMReX done!"