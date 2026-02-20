**GPIO LDD**

GPIO Control Driver (Interrupt, poll, ioctl) Project Overview This project implements a Linux character device driver for controlling and monitoring GPIOs using: Hardware interrupts (IRQ) for button presses Blocking read using wait queues Non‑blocking I/O using poll() ioctl() interface for control operations A user‑space menu‑driven C application is provided to demonstrate all driver features.
