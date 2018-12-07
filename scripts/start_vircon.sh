#!/bin/bash

if lsmod | grep -q vircon; then 
  echo vircon module already loaded
else
  echo loading module
  modprobe vircon vircon_enable=1
fi

if [ -e /var/run/fbvncserver.pid ]; then 
  echo fbvncserver already running
else
  echo setting mode and starting fbvncserver
  fbset -xres 1440 -yres 900 -depth 16
  /usr/local/bin/fbvncserver -k /dev/input/event1 -t /dev/input/event2
fi
