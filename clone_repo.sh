#!/usr/bin/env bash
git clone https://Keith-Bateman@github.com/Keith-Bateman/hdf5.git .
git remote add canonical https://Keith-Bateman@github.com/HDFGroup/hdf5.git
git remote set-url canonical --push "DO NOT PUSH TO CANONICAL"
git fetch canonical
git checkout canonical/develop
