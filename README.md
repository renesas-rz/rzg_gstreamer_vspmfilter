# vspmfilter

This is a GStreamer plugin that can use the color conversion and
scaling with the VSPM hardware acceleration on Renesas SoC.

Currently the following boards and MPUs are supported:
- MPU: R9A07G044L (RZ/G2L)
- MPU: R9A07G044C (RZ/G2LC)
- MPU: R9A07G043U (RZ/G2UL)
- MPU: R9A07G054L (RZ/V2L)

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

