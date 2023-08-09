#!/bin/bash

umask 000
sudo /usr/local/spdk/bin/spdk_tgt -m0x03 -r /tmp/iscsi_tgt.sock  > ./iscsi_tgt.log 2>&1

