# nanomsg-transport-ofi (1.1.0b)

Nanomsg transport for interfacing with the OpenFabrics Interfaces (OFI) Library (libfabric).

## Known Problems

 1. The current version handles only IPV4 address families.

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

The following copy-paste-friendly instructions can help you getting started:

 1. Make sure you have [libfabric](http://ofiwg.github.io/libfabric/) installed in your system.
 2. Check-out the [customized nanomsg sources from wavesoft](https://github.com/wavesoft/nanomsg) (until they are available from upstream) and prepare the autogen scripts:

    ```sh
    git clone -b pull-nn_allocmsg_ptr https://github.com/wavesoft/nanomsg
    NANOMSG_SROUCES=$(pwd)/nanomsg
    cd $NANOMSG_SROUCES; ./autogen.sh; cd ..
    ```

 3. Check-out `nanomsg-transport-ofi`:

    ```sh
    git clone https://github.com/wavesoft/nanomsg-transport-ofi.git
    cd nanomsg-transport-ofi
    ```
 4. Patch the sources of nanomsg, using the `patch-nanomsg.sh` script that comes with this project.

    ```sh
    ./patch-nanomsg.sh $NANOMSG_SROUCES
    ```
 5. You can now compile nanomsg:

    ```
    cd $NANOMSG_SROUCES
    ./configure --enable-ofi && make
    sudo make install
    ```
