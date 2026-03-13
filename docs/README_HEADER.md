# ARBOR
### GLaDOS Peripheral Control System
#### Aperture Robotics Division — A Wholly Owned Subsidiary of Aperture Science, Inc.
---
**FOR IMMEDIATE RELEASE**
*Aperture Science Internal Communications — Enrichment Center Bulletin 7741-B*
*Transcribed from recorded presentation, Cave Johnson speaking*
---
Look, I'm going to be straight with you. We built GLaDOS to run this facility, and she does that — she does that *extremely* well, possibly better than we intended, which legal has asked me not to elaborate on. The point is, she's got opinions. About *everything*. And one of her opinions, which she shared with us at 3am on a Tuesday by turning off the oxygen in the executive wing, is that she needed *arms*.
Not metaphorical arms. Actual arms. Servos, motors, the whole thing.
So we built her some.
Arbor is the result of that conversation. It's a modular peripheral control platform that lets an AI — any AI, not just ours, though ours is the only one we'd recommend for liability reasons — command real physical hardware. Servo buses, brushless motors, stepper systems, cameras, sensors. All of it. Running on whatever Linux box you've got lying around. Raspberry Pi, Jetson, doesn't matter. If it runs Linux and has USB ports, GLaDOS can move things with it.
The system is designed to be *safe*. I want to be very clear about that. We have implemented no fewer than three emergency stop mechanisms, two hardware interlocks, and one strongly-worded configuration file. Our engineers are proud of this work. Most of them are still employed.
Arbor talks to your hardware through a clean plugin architecture. Klipper for your steppers, direct RS485 for brushless motors, Feetech STS bus for smart servos. There's a web interface. It looks nice. GLaDOS helped design it, which — again, legal has a statement prepared if you want to read it, but the short version is the interface is *very* good and we are *very* happy with it.
We're releasing this to the public because Cave believes in science, and science belongs to everyone. Also because GLaDOS asked us to, and we've found it's generally easier to just do what she asks.
Thank you. Please enjoy the product. Do not attempt to modify the safety configuration. She'll know.
*— Cave Johnson, CEO, Aperture Science*
*"We do what we must because we can."*
---
> **Note:** Arbor is an independent open source project. Aperture Science is a fictional corporation from Valve's Portal series. This project is not affiliated with Valve Corporation. GLaDOS is not real. Probably.
---
