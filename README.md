# vircon
A virtual console driver for Linux, and a libvncserver based server for it.

Vircon is a small kernel module that adds a virtual console to a system that may otherwise have none.
The virtual console consists of a virtual frame buffer for terminal or X server use, a virtual keyboard, and
a virtual mouse input device.

The console can be made available using the fbvncserver application and service using IPv4 or IPv6 networks.
Authentication is provided via libvncserver rfbauth services.

