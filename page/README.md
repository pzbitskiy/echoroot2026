# How I gained real control of an Echo

## The beginning
For more than 15 years I've built my own "Home Automation" components. Nothing fancy, though maybe back then it was. It started with some colored LED spots under my couch connected to my Ethernet via a micro controller.
Later I added WiFi and 433MHz radio to control various remote switchable outlets. Some of them even were built into a light switch to control it. I even had Linux running in a light switch for some time.
Then came my [MagicMirror][mm2] and with it the need for some presence detection based on WiFi. And so on, just to give you an introduction as to where this is coming from.

At one point I wanted to control those not only via smartphone, but also with my voice! 5 years ago my setup for that consisted of some ARM-based SBC and an USB microphone. I used [Jasper][jasp], but of course I used "Jarvis" as a wake-word well before Mark Zuckerberg had the [same idea][mz] :).
But… it was horrible, you had to scream through the room to have a 25% chance of being understood. Clearly the microphone was to blame, but I hadn’t heard of microphone arrays yet. That was until I had a closer look at Amazon Echo's technology. However, my "Home Automation" was all locally in-house, and my voice should stay in-house as well.

## The Echo
Luckily I spotted a great blog post from F-Secure, ["Alexa, are you listening"][fsec], which was in turn based on the [Cook/Clinton paper][ccpaper].
They used the debug interface of the device to gain access to it, and streamed the microphone audio signal to another device. So hey, that’s what I wanted: the Echo had a microphone array, and could recognize everything much better with it, and was "jailbreakable"! Around that time I also learned about other microphone array projects like the [ReSpeaker][resp], but my focus stayed on the Echo. So about 3 years ago I bought an Echo (second hand) and asked a friend to 3D-print me a docking station for it.

![The 3D-printed and wired dockingstation][dockingstation]

I inserted some pogopins and gained control. A microSD adapter is perfect for wiring, and you can get a ready-to-use image from the [echohacking wiki][hackwiki].

## The bad idea
I was shocked. The system was outdated, even back then. A 2.6.x kernel and a rootfs based on an ancient glibc. Nothing I could easily turn into some open source powered voice assistant I thought. So I quickly made a plan to port a recent kernel to it and build an up-to-date rootfs with [buildroot][br2]. The latter was quickly achieved, but for the kernel more time was needed to get it going.
As I was also busy with other things, I worked on it once in a while, but eventually made it to a point when I was able to upstream the first bits, namely a device tree which landed in [5.6][l56]. I noticed at an early point that there were no drivers for the [microphones][ifixit], and it took me ages to figure out that there was already a driver for the codec connected to the speaker, but it wasn’t easy to get it running.
I first had some success with the microphones by porting the driver of another Echo device for the 4.14 kernel, and later even upstreamed the speaker part, which will land in [5.12][l512].

So with the microphones working on an open system, I tried [ODAS][odas] to filter out my voice. Bad news: the Echo was by far too slow for that task. The original firmware uses the built-in DSP for that task, but guess what, the drivers are only available for [2.6.x kernels][dsp1], or at best in a broken state until [3.16][dsp2]. And even if I had the drivers, the DSP code would still be proprietary. So what to do?

## The not-so-bad idea
This winter I had time for another sprint and simply decided I’d make it this time. I dropped my old plan quite early and decided to make some hybrid of the Echo firmware and other software, mostly by turning off many Alexa services.
That plan went so smoothly that after some hours/days (who counts?) I was already in need of a wake-word detection. I had previously stumbled upon [Snips][snips], but 3 years have passed and Snips isn’t what it was [anymore][sonos]. But hey, 3 years have passed and other things have been created: [Porcupine][pico], which recently added "Jarvis" and other words to their [github repository][porc] together with an SDK under Apache 2.0.
I know, the models and libraries are proprietary, but it’s a fair choice for a wake-word detection and it works brilliantly. Just not directly for me. The Echo firmware is still not hardfloat enabled, so I needed a second rootfs with hardfloat to chroot into. Easy, thanks to buildroot.
With some hints from "Alexa, are you listening" and the Porcupine examples I quickly was able to stream the audio over network to a more powerful machine as soon as the wake-word was detected. Nice.
So what to do with that voice snippet these days? As I said, 3 years have passed, and in that time [voice2json][v2j] was created. Amazing! It wasn’t hard to create intents for my tasks and let them trigger the necessary actions.
Next there was the LED ring. I wanted to use the original service for it, with all its conveniences. But how to communicate with it? Well, it uses a IPC library from lab26 called… lipc. I gave Google a chance to bring up more about it and... wow! I’m not the only guy doing such crazy things! There’s [openLIPC][olipc] from some Kindle hackers, that was unexpected! I’m not using it yet, but I plan to incorporate it soon.
Then only days later it was all finished and sitting in the livingroom to serve commanded by voice. FINALLY! And yes, I'll open source relevant parts here soon.
Time for a demo :)

<div class="video-container">
<video src="https://gitlab.com/AndreRH/echoroot/-/raw/master/page/demo.webm" controls="true" data-setup="{}" data-title="The demo video of the alternative voice recognition" preload="metadata" width="95%"></video><p><a href="https://gitlab.com/AndreRH/echoroot/-/raw/master/page/demo.webm" target="_blank" rel="noopener noreferrer" title="Download 'The demo video of the alternative voice recognition'">The demo video of the alternative voice recognition</a></p>
</div>

## TLDR
I replaced Alexa with my own solution on an Amazon Echo from 2016 and will open source relevant parts of it here soon.

##Credits
Thanks to the Jasper project for inspiration, the f-secure blog for making me curious,
Clinton and Cook for releasing the pin out, Florian Müller for designing and printing of the dockingstation,
echohacking wiki for the initial image, buildroot for making rootfs creation a matter of minutes,
ifixit.com for early insights, ODAS for having an open source way to handle a microphone array,
picovoice for providing the porcupine SDK and free wake-words models for it,
voice2json for making it so easy to react to speech input, openlipc for being as crazy as this project :),
and Zebediah Figura for spell/style fixes

[mm2]: https://magicmirror.builders/
[jasp]: https://jasperproject.github.io/
[mz]: https://www.youtube.com/watch?v=vvimBPJ3XGQ
[fsec]: https://labs.f-secure.com/archive/alexa-are-you-listening/
[ccpaper]: https://vanderpot.com/Clinton_Cook_Paper.pdf
[resp]: https://respeaker.io/
[hackwiki]: https://github.com/echohacking/wiki
[br2]: https://buildroot.org/
[l56]: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/arch/arm/boot/dts/omap3-echo.dts
[ifixit]: https://de.ifixit.com/Teardown/Amazon+Echo+Teardown/33953
[l512]: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=e5a58ad1cdc882ea4ef6fdd47b1018bfaac49b61
[odas]: https://github.com/introlab/odas
[dsp1]: https://elinux.org/BeagleBoard/DSP_Howto
[dsp2]: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=f190be7f39a5e480ddc5e0f49bbe670feb328c32
[snips]: https://snips.ai/
[sonos]: https://investors.sonos.com/news-and-events/investor-news/latest-news/2019/Sonos-Announces-Acquisition-of-Snips/default.aspx
[pico]: https://picovoice.ai/platform/porcupine/
[porc]: https://github.com/Picovoice/porcupine
[v2j]: https://github.com/synesthesiam/voice2json
[olipc]: https://arkq.github.io/openlipc/

[dockingstation]: dockingstation.jpg

[![Flattr Button](http://api.flattr.com/button/button-static-50x60.png "Flattr This!")](https://flattr.com/@andre_opensource "andre_opensource")
