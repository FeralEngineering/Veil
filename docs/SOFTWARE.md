# Software

Veil runs on an ESP32-CAM and handles image capture, processing, storage, and local archive hosting.

The firmware follows this flow:

```text
capture → process → save → serve
```

---

## Capture

Images are captured in:

```text
RGB565
320x240 (QVGA)
```

QVGA is used for memory stability.

Higher resolutions caused memory fragmentation and inconsistent capture behavior during testing.

Captured frames are converted to RGB888 before processing.

---

## Processing Pipeline

Each image passes through a fixed sequence of processing stages:

1. Warp distortion  
2. Shear transform  
3. Swirl transform  
4. Drift displacement  
5. Glow extraction  
6. Color grading  
7. Contrast compression  
8. Shadow boosting  
9. Chromatic channel offset  
10. Vignette falloff  
11. Blur blending  

Each stage modifies the active framebuffer before the final JPEG is written.

The original frame is not retained.

---

## Storage

Processed images are written to the onboard microSD card.

Naming format:

```text
veil_00000001.jpg
```

File numbering is stored in ESP32 NVS using `Preferences`, allowing numbering to continue across reboots.

---

## Web Interface

Veil creates its own access point:

```text
SSID: VeilCam
PASS: veilveilveil
```

Runs a local web server at:

```text
http://192.168.4.1
```

Routes:

| Route | Function |
|---|---|
| `/` | Main interface |
| `/status` | Device status |
| `/capture` | Trigger capture |
| `/list` | List stored images |
| `/img` | Serve image files |

---

## Button Logic

GPIO13 is polled in the main loop.

A button press sets a capture request flag.

Capture and processing are handled in a separate FreeRTOS task to keep the web interface responsive.

---

## LED Logic

GPIO4 shares the onboard flash LED circuit.

During capture it remains high-impedance to avoid camera bus conflicts.

After a successful save, the LED pulses briefly as a status indicator.
