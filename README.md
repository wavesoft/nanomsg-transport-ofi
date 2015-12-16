# nanomsg-transport-ofi (0.1.0a)

Nanomsg transport for interfacing with the OpenFabrics Interfaces (OFI) Library (libfabric).

__WARNING: This is an early alpha release! This project is currently unoptimised and should not be used in production!__

## Known Problems

 1. The current version handles only IPV4 address families.
 2. `usnic` provider does not seem to support the `FI_SHUTDOWN` event and therefore a `bound` socket will run out of file descriptors if many connections are attempted to be established. 
 3. The current implementation breaks the zero-copy principle [here](src/transports/ofi/sofi.c#L245) and [here](src/transports/ofi/sofi.c#L342), and therefore has some performance penalties.
 4. Currently, it seems that with `usnic` you can only have up to 16 concurrent connections.

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
