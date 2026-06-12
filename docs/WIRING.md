# Wiring

## Button

```text
GPIO13 ---- Momentary Button ---- GND
```

Configured with internal pull-up.

Pressing the button pulls the pin low and queues a capture.

---

## LED

```text
GPIO4 ---- LED ---- GND
```

Used as a status pulse after capture completes.

GPIO4 is not used during active capture because it shares the flash circuit and caused camera instability.

---

## Storage

```text
MicroSD slot (onboard ESP32-CAM)
```

Used for storing captured images.

Files are written sequentially:

```text
veil_00000001.jpg
veil_00000002.jpg
veil_00000003.jpg
```

The file index is stored in NVS and persists between reboots.

---

## Power

During development:

```text
ESP32-CAM MB Programmer -> USB
```

For standalone use:

```text
5V -> ESP32-CAM 5V / GND
```

The board handles 3.3V regulation internally.
