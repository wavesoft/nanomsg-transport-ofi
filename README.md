# nanomsg-transport-ofi

Nanomsg transport for interfacing with the OpenFabrics Interfaces (OFI) Library (libfabric).

__This project is under development and might not compile and/or work!__

## How to compile

 1. Make sure you have [libfabric](http://ofiwg.github.io/libfabric/) installed in your system.
 2. Download the [latest sources of nanonsg](https://github.com/nanomsg/nanomsg/releases):
```sh
wget https://github.com/nanomsg/nanomsg/releases/download/0.8-beta/nanomsg-0.8-beta.tar.gz
tar -zxf nanomsg-0.8-beta.tar.gz
NANOMSG_SROUCES=$(pwd)/nanomsg-0.8-beta
``` 
 3. Check-out `nanomsg-transport-ofi`:
```sh
git clone https://github.com/wavesoft/nanomsg-transport-ofi.git
```
 4. Patch the sources of nanomsg, using the `patch-nanomsg.sh` script that comes with this project.
```sh
./patch-nanomsg.sh $NANOMSG_SROUCES
```
 5. You can now compile nanomsg:
```
cd $NANOMSG_SROUCES
./configure && make
sudo make install
```
