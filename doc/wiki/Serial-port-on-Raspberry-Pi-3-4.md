1. Run the configuration tool

```
sudo raspi-config
```

2. Navigate to:

> Interface Options -> Serial Port 

* Select `No` for login shell.
* Select `Yes` to enable serial port hardware.

3. Edit the boot configuration file

```
sudo nano /boot/firmware/config.txt
or (if not exists)
sudo nano /boot/config.txt
```

3.1. Add the following line at the bottom:  

```
dtoverlay=disable-bt
```

4. Disable the Bluetooth service

```
sudo systemctl disable hciuart
```

