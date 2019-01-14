# esp32_powermon

Adding a web front end to your household power meter.

(c) 2019 Mike Field <hamster@snap.net.nz>

Most modern power meters have a "pulse per Watt-Hour" LED. If you optically sense 
this pulse you can count them and measure your household power usage.

This project for the ESP32 uses a Light Dependant Resistor (LDR) to sense the pulses, 
and serves a small webpage to draw a graph of the last 24 hours.

NOTE: Includes code from https://github.com/igrr/esp32-http-server for the web server.

See an example of the output in powermon_graph.png.

Using this:
===========
1. The web page is viewable using  http://<assigned ip address>/index.html
  
2. The sensor is an Cadmium Sulphide Light dependant resistor, buffered with
a transistor configured as an emmiter follower. Ping me an email if you want
a schematic and/or photos.The sensor should be connected to the A0 input.

Method of operation
===================
The program has three tasks.

1. One task uses the I2S interface to read the ADC and look for pulses from the 
sensor. LDRs are quite slow to respond and recover, and you need to allow for
ambient light, so this uses an ADC rather than a digital input. When it detects 
a pulse it updates a counter

2. The second task keeps an eye on the clock, and samples the counter every two
minutes and passes it on to update the graph data.

3. The third task is the web server, that serves a simple HTML/Javascript page 
as "index.html". This shows just the last 720 samples.

There is a bit of a race condition between process 2 and 3, but nothing that will cause any issues.

Possible Enhancements
=====================
- Add NTP to allow the time to be obtained from the network (or maybe through the web interface?)

- Add a history table for the last 24 days

- Save data to storage for recovery after a reboot/reset (uSD card?)

- Improve Javascript that draws the graph

- Also push the data up to a web service.
