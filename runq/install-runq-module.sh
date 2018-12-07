#!/bin/bash

cd /root/vircon

apt-get update
apt-get -y install kmod gcc make libvncserver-dev

KERN="4.15.0-42-generic"

apt-get -y install linux-modules-$KERN 
apt-get -y install linux-headers-$KERN 

KERNELVER=$KERN make && KERNELVER=$KERN make install
