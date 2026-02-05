# BMP280 I2C Sensor – Linux Userspace Driver (C)

This project demonstrates interfacing the **BMP280 temperature and pressure sensor**
with a Linux-based system using the **I2C bus** from **userspace in C**.

The application communicates with the sensor via `/dev/i2c-*`, reads calibration
data from the sensor registers, applies Bosch-recommended compensation formulas,
and continuously prints temperature and pressure values.

---

## 📌 Features

- Linux **userspace I2C communication**
- Reads and verifies **BMP280 Chip ID**
- Reads **factory calibration data**
- Calculates **compensated temperature & pressure**
- Continuous sensor polling
- Portable across Linux SBCs (Raspberry Pi, BeagleBone, etc.)

---

## 🔌 Hardware Requirements

- BMP280 Sensor Module
- Linux-based board (Raspberry Pi / BeagleBone / Embedded Linux)
- I2C interface enabled

### BMP280 I2C Connections

| BMP280 Pin | Board Pin |
|-----------|----------|
| VCC       | 3.3V     |
| GND       | GND      |
| SDA       | I2C SDA  |
| SCL       | I2C SCL  |

---

## 🧠 Software Requirements

- Linux OS
- GCC compiler
- I2C enabled in kernel
- I2C tools (optional for debugging)

```bash
sudo apt install i2c-tools
