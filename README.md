# esp32_powermon
Adding a web front end to your household power meter.

Most modern power meters have a "pulse per Watt-Hour" LED. If you optically sense 
this pulse you can count them and measure your household power usage.

This project for the ESP32 uses a Light Dependant Resistor (LDR) to sense the pulses, 
and serves a small webpage to draw a graph of the last 24 hours.
