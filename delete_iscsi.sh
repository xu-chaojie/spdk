#!/bin/bash
vol=$1

target=iqn.2016-06.io.spdk:${vol}
sudo /home/curve/spdk/scripts/rpc.py -s/tmp/iscsi_tgt.sock iscsi_delete_target_node $target

ocf=ocf_${vol}
sudo /home/curve/spdk/scripts/rpc.py -s/tmp/iscsi_tgt.sock bdev_ocf_delete $ocf

cbd_bdev=cbd_${vol}
sudo /home/curve/spdk/scripts/rpc.py -s/tmp/iscsi_tgt.sock bdev_cbd_delete $cbd_bdev

memdisk=Malloc_${vol}
sudo /home/curve/spdk/scripts/rpc.py -s/tmp/iscsi_tgt.sock bdev_malloc_delete $memdisk

