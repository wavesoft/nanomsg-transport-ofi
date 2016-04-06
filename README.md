# nanomsg-transport-ofi (1.0.0b)

Nanomsg transport for interfacing with the OpenFabrics Interfaces (OFI) Library (libfabric).

## Known Problems

 1. The current version handles only IPV4 address families.
 2. The keepalive mechanism is not yet implemented in the current state of the devel branch

## How to use

After patching your nanomsg sources, it will make the `ofi` transport available for use. This endpoint format is `ofi://[IP Address]:[Port]`.

For example, you can test the `ofi` transport with the `nanocat` utility that comes with `nanomsg`.

```
~$ nanocat --pull --bind ofi://127.0.0.1:5050 -A&
OFI: Using fabric=IP, provider=sockets
~$ nanocat --push --connect ofi://127.0.0.1:5050 -D Hello
OFI: Using fabric=IP, provider=sockets
Hello
```

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
