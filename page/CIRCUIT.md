# The electronics part

## What you need
I recommend to have a USB to UART (TTL) converter. I used a "D-SUN USB to TTL" [in red][dsun], which also provides 3.3V and 5V output.
For the SD card you will want to use a microSD for the image, but a microSD to SD card adapter for soldering.
And yes, you need a soldering iron. It's possible to avoid it, but that makes things more complicated I think.
For connecting the Echo itself you can go with pogo-pins and some 3D printing, or be on the safe side and solder wires on it as well.

## The circuit
That's how it could be done, just note that you also can power the microSD from another source like the 3.3V of a USB to UART converter:

![Wiring the Echo][wiring]

The circle is supposed to mark the edge of the Echo when viewed from below.

Thanks to Kent for discovering the 3.3V output on TP19!

[dsun]: https://www.cpmspectrepi.uk/raspberry_pi/MoinMoinExport/USBtoTtlSerialAdapters.html

[wiring]: wiring.png

[![Flattr Button](http://api.flattr.com/button/button-static-50x60.png "Flattr This!")](https://flattr.com/@andre_opensource "andre_opensource")
