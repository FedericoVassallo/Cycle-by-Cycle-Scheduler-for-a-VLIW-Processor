#!/bin/bash
g++ -std=c++17 -O2 -o scheduler \
    src/main.cpp \
    src/data_structures.cpp \
    src/dependency.cpp \
    src/io_handler.cpp \
    src/scheduler.cpp \
    src/register_allocation.cpp \
    -Iinclude