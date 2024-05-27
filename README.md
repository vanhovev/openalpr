# Build OpenALPR
1. Install prerequisites
```
sudo apt install -y libopencv-dev libtesseract-dev git cmake build-essential libleptonica-dev liblog4cplus-dev libcurl3-dev
```
1.1. Install beanstalkd to use `alprd` (to use as daemon for video stream etc.)
```
sudo apt install beanstalkd
```
2. Clone the latest code from GitHub
```
git clone https://github.com/openalpr/openalpr.git
```
3. Setup the build directory
```
cd openalpr/src
mkdir build
cd build
```
4. setup the compile environment
```
cmake -DCMAKE_INSTALL_PREFIX:PATH=/usr -DCMAKE_INSTALL_SYSCONFDIR:PATH=/etc ..
```
5. compile the library
```
make
```
6. Install the binaries/libraries to your local system (prefix is /usr)
```
sudo make install
```

7. Edit configuration /etc/openalpr/alprd.conf
```
; This configuration file overrides the default values specified
; in /usr/share/openalpr/config/alprd.defaults.conf
[daemon]

; country determines the training dataset used for recognizing plates.
; Valid values are: us, eu, au, auwide, gb, kr, mx, sg
country = eu
pattern = fr

; text name identifier for this location
site_id = openalpr            

; Declare each stream on a separate line
; each unique stream should be defined as stream = [url]

; Example stream config:
stream = http://admin:Azerty1234!@192.168.1.108/cgi-bin/mjpg/video.cgi?subtype=1
;   stream = http://127.0.0.1/example_second_stream.mjpeg
;   stream = webcam

; Number of threads to analyze frames.
analysis_threads = 4

; topn is the number of possible plate character variations to report
topn = 10

; Determines whether images that contain plates should be stored to disk
store_plates = 0
store_plates_location = /var/lib/openalpr/plateimages/

; upload address is the destination to POST to
upload_data = 0
upload_address = http://localhost:9000/push/

```
