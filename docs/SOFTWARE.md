# Software

Veil runs on an ESP32-CAM and handles capture, processing, storage, and local archive hosting.

The firmware is built around a simple flow:

capture → alter → save → serve

---

## Capture

Images are captured in:

```text
RGB565
320x240 (QVGA)
```

QVGA was chosen for stability.

Higher resolutions caused memory fragmentation and unreliable behavior.

After capture, the image is converted to RGB888 for processing.

---

## Processing Pipeline

Each image passes through the following stages:

1. Smooth Warp  
2. Rift Shear  
3. Portal Swirl  
4. Veil Drift  
5. Glow Extraction  
6. Pink/Fog Grade  
7. Depth Lie  
8. Shadow Emission  
9. Chromatic Split  
10. Vignette  
11. Soft Blur  

Each pass modifies the image before it is written to storage.

The original frame is discarded.

---

## Storage

Images are saved to the onboard microSD card.

Naming format:

```text
veil_00000001.jpg
```

File numbering is stored in NVS (`Preferences`) so numbering continues after reboot.

---

## Web Interface

Veil creates its own access point:

```text
SSID: VeilCam
PASS: veilveilveil
```

Runs a local web server on:

```text
http://192.168.4.1
```

Routes:

| Route | Function |
|---|---|
| `/` | Main UI |
| `/status` | Device status |
| `/capture` | Queue capture |
| `/list` | List saved images |
| `/img` | Serve saved image |

---

## Button Logic

GPIO13 is monitored in the main loop.

Pressing it queues a capture request.

Captures are processed in a separate FreeRTOS task to keep the web interface responsive.

---

## LED Logic

GPIO4 is left high-impedance during capture.

After successful save, it pulses briefly.

This avoids conflicts with the camera flash circuit.
