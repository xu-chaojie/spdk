#!/bin/bash
vol=$1
user=$2
cacheMB=$3

memdisk=Malloc_${vol}
sudo /home/curve/spdk/scripts/rpc.py -s/tmp/iscsi_tgt.sock bdev_malloc_create -b $memdisk $cacheMB 512

cbdpath=//${vol}_${user}_
cbd_bdev=cbd_${vol}
sudo /home/curve/spdk/scripts/rpc.py -s/tmp/iscsi_tgt.sock bdev_cbd_create -b $cbd_bdev --cbd ${cbdpath} --exclusive=0 --blocksize=4096

ocf=ocf_${vol}
sudo /home/curve/spdk/scripts/rpc.py -s/tmp/iscsi_tgt.sock bdev_ocf_create $ocf wb $memdisk $cbd_bdev

lun_pair=${ocf}:0
sudo /home/curve/spdk/scripts/rpc.py -s/tmp/iscsi_tgt.sock iscsi_create_target_node $vol $vol $lun_pair 1:2 1024 -d



