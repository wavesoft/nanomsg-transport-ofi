
# Utility Scripts

This folder contains various utility scripts related to testing or using the nanomsg OFI transport.

## run-on.sh

This script runs a command line with it's afinity configured to the CPU cores the NIC sends IRQs to. Usage:

```
run-on.sh [interface] [command-line ...]
```
