### Installing the vircon driver in runq 

1. Disable module signature checking in runq
2. For simplicity clone this repo in /root (to /root/vircon)
3. Edit the install script and set the KERN variable to the kernel version inside runq.
4. run `docker run  --rm -ti -v /root/vircon:/root/vircon -v /var/lib/runq/qemu/lib/modules:/lib/modules ubuntu /root/vircon/runq/install-runq-module.sh`

This installs a set of modules to runq.
