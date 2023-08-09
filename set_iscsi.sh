#!/bin/bash

protal_ip=$1

#sudo ./scripts/rpc.py -s/tmp/iscsi_tgt.sock bdev_malloc_create -b Malloc0 10240 512

#创建curve bdev，名字是由变量B设置，卷路径，是否共享，blocksize
#sudo ./scripts/rpc.py -s/tmp/iscsi_tgt.sock bdev_cbd_create -b curve_bdev1 --cbd "//test1_curve_" --exclusive=0 --blocksize=4096


#创建名字为myocf的bdev，这里实际是创建一个OpenCAS bdev用来缓存我们的curvebs bdev，回写模式wb，缓存设备Malloc0，后端设备是curvebs bdev
#sudo ./scripts/rpc.py -s/tmp/iscsi_tgt.sock bdev_ocf_create ocf1 wb Malloc0 curve_bdev1

#创建侦听端口组1:
sudo ./scripts/rpc.py -s/tmp/iscsi_tgt.sock iscsi_create_portal_group 1 ${protal_ip}:3260

#创建客户端地址组2：
sudo ./scripts/rpc.py -s/tmp/iscsi_tgt.sock iscsi_create_initiator_group 2 ANY 10.0.0.0/8

#创建scsi target, 附带两个lun, lun1是myocf，另外一个是Malloc1, 访问受限设置(侦听端口组1允许客户端地址组2访问), -d不使用chap
#sudo ./scripts/rpc.py -s/tmp/iscsi_tgt.sock iscsi_create_target_node disk1 "Data Disk1" "ocf1:0" 1:2 1024 -d


