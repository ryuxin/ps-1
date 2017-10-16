#!/bin/bash

touch ./pfwr_r
sudo sh ./test.sh
scp ./pfwr_r ryx@192.168.0.1:/home/ryx
rm ./pfwr_r
