# Veil

![Veil live](images/veil-live.jpg)

## Hardware

| | |
|---|---|
| ![](images/veil-idle.jpg) | ![](images/veil-back.jpg) |
| ![](images/veil-side.jpg) |  |

Veil is a handheld ESP32-CAM built around an idea I’ve had for a long time:

what if a camera could catch things moving between dimensions?

Not in a paranormal ghost-hunting way. More like interference. Overlap. Things crossing through each other for a second and leaving traces.

That was the whole point of building it.

You take a photo, but before you ever see it, Veil drags it through its own internal process and saves only the altered version.

The untouched image never survives.

Every capture gets bent, smeared, shifted, fogged, fractured, and warped in different ways. Sometimes subtly, sometimes hard.

The idea is that if something *was* there, and it wasn’t meant to fully resolve in our space, this is maybe closer to how it would actually show up.

Or maybe it just makes weird images.

Either way, that’s the experiment.

---

## Using it

Press the button.

That takes a single capture.

The image gets processed and saved to SD.

Veil hosts its own Wi-Fi archive so you can go back and look through what it’s collected.

```text id="veilwifi"
SSID: VeilCam
PASS: veilveilveil
```

Open:

```text id="veilip"
http://192.168.4.1
```

From there you can trigger new captures and browse old ones.

---

## Hardware

- AI Thinker ESP32-CAM  
- microSD storage  
- momentary trigger button  
- status LED  
- repurposed disposable vape box enclosure  

More detailed hardware and wiring docs are in `/docs`.

---

## Repo structure

```text id="veiltree"
Veil/
├── README.md
├── firmware/
│   └── veil.ino
├── docs/
│   ├── HARDWARE.md
│   ├── WIRING.md
│   ├── SOFTWARE.md
└── images/
```
