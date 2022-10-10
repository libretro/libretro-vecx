vecx
====

Compiling
------------
for libretro build, make sure to set -DCMAKE_BUILD_TYPE=Libretro

Requirements
------------
(for standalone port)
* `libsdl`
* `sdl_gfx`
* `sdl_image`

Authors
-------

* Valavan Manohararajah - original author
* [John Hawthorn](https://twitter.com/jhawthorn) - SDL port
* [Nikita Zimin](https://twitter.com/nzeemin) - audio
* "Demeth" - libretro port
* dave j - OpenGL rendering

Version 1.5 changes
-------------------
By Malban - (http://vide.malban.de/).

* fixed diverse emulation hickups
	* SHIFT register is finaly shifting correctly (every second cycles instead of every first)
    * cycle exacter handling of analog hardware
	* added drift emulation (although you better leave it off)
	* added better analog timings (e.g. Clean Sweep display better)
	* 6809: clr instruction also READS memory
	* 6809: FAST RTI, CC flag was handled buggy
	* 6809: exg could provide wrong results in Reg A
	* VIA: interrupt flag for CA1 were not set in accordance to the CRTL register
	* VIA: T2 timer shift clock speed doubled
	* VIA: T2 low counter iFlag was not set correctly (Omega Chase)
	* VIA: T1 low counter iFlag was not set correctly (Vector Patrol)
  
* added sample support "OH NO SPIKE!"
* corrected bankswitching, full support for "normal" PB6 bankswitching
* also added support for Vectorblade bankswitchin IRQ and PB6
* added "dummy" emulation of flash support for Vectorblade
* saves in Vectorblade are actually written to the "bin" file loaded!!!
* added support for SpritesTM loading mechanism -> support for e.g. BadApple
  e.g. badapple.bin file (is about 200-300 byte) requires a movie file named like the bin 
  with the extension ".vvm" in the same directory
* added support for "chassis" loading. The chassis must be in retroarch ".../overlay/VecX/screen.png" 
  For this to work properly the display must be "hardware display" and use openGL.
* added support for "auto_sync" (cleaner display where is works) (synced with T2 timer...)
* added some configuration options  
* changed the aspect ratio of the display and "shrinked" it a bit

![Vectorblade](http://vectrex.malban.de/libretro/LibRetro_VB.png)
![CleanSweep](http://vectrex.malban.de/libretro/LibRetro_Sweep.png)
![BadApple](http://vectrex.malban.de/libretro/LibRetro_BadApple.png)
