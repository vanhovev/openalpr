1. Install `ccmake` :

```
sudo apt-get install cmake-curses-gui
```

2. Launch `ccmake` :

```
cd ~/openalpr/src/build
ccmake ..
```

3. Modify missing paths:

To modify a field, press Enter.
Set the following paths:

* Tesseract_INCLUDE_CCMAIN_DIR : /usr/include/tesseract
* Tesseract_INCLUDE_CCUTIL_DIR : /usr/include/tesseract

Press Enter again to confirm.

4. Configure and generate:

Press c to configure.
If everything is correctly configured, press g to generate and exit.

5. Continue the build process (step 4 from the installation page - setup the compile environment) :

