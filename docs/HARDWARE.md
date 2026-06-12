# Hardware

## Core Components

- **AI Thinker ESP32-CAM**
- **ESP32-CAM MB programmer**
- **MicroSD card**
- **Momentary pushbutton**
- **Yellow LED**
- **Plastic disposable vape packaging (used as enclosure)**

---

## Pin Use

| Pin | Function |
|---|---|
| GPIO13 | Capture trigger |
| GPIO4 | Status LED |

---

## Notes

GPIO4 shares the onboard flash circuit.

Using it during capture caused camera failures during development.

Final firmware leaves GPIO4 inactive during image capture and only pulses it afterward.

The enclosure is the original plastic box from a disposable vape, modified to fit the board, button, and LED.
