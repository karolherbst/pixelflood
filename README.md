# pixelflood
super high performance pixelflut server

# hardware requiernments
* fast network
* fast CPU
* GPU capable of displaying an ARGB8888 texture

# software requiernments
* libevent2
* SDL2
* SDL2_TTF

# Usage
Just start it. The resolution and the port can be configured through compile time constants inside main.c. With SDL2 2.0.6 it's not even required to have X11 running. Just start it from your tty and be happy.

To quit simply press 'Q'. If running from the console, the running user needs access to the "/dev/input/" device files or otherwise there is no way to kill the server (except by using SSH or something)

# performance
The Code is tuned for performance as much as possible and doesn't add "fancy" features like network load balancing or other things which would impact performance in a negative way.

Networking is done event based through libevent2

The pixelbuffer is displayed through SDL2.

I was able to achieve around 22 Gbit/s or 135 MPixel/s on an Intel i7-7700HQ CPU with threaded clients running on the same machine.

One can disable all those fancy spectre/meltdown mitigations to further increase performance, just boot with
* amd64: "pti=off spectre_v2=off noibpb noibrs spec_store_bypass_disable=off"
* arm64: "kpti=off"
* ppc64el: "no_rfi_flush nopti spec_store_bypass_disable=off"
