#!/bin/bash

cmake --preset="Linux Config"
cmake --build --preset="Linux Debug Build"
cmake --build --preset="Linux Release Build"
