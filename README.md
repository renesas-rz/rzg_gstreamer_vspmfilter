# vspmfilter

This is a GStreamer plugin that can use the color conversion and
scaling with the VSPM hardware acceleration on Renesas SoC.

Currently the following boards and MPUs are supported:
- MPU: R8A774C0 (RZ/G2E)
- MPU: R8A774A1 (RZ/G2M v1.3) and R8A774A3 (RZ/G2M v3.0)
- MPU: R8A774B1 (RZG2N)
- MPU: R8A774E1 (RZG2H)

## License

- Copyright (C) 2014-2023 Renesas Electronics Corporation

- gstreamer-vspmfilter is available under the terms of
the GNU Lesser General Public License v2.0

## Build instruction

First, vspm-user-module and kernel-module-vspm must be available.
Run the following scripts (self-compile)

``` bash
$ source <Renesas SDK>
$ autoreconf -vif
$ ./configure --build=x86_64-linux --host=aarch64-poky-linux --target=aarch64-poky-linux --prefix=<Destination>
$ make
$ make install
```

