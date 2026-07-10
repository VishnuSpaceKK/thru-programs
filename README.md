# thru-counter

A Thru blockchain program written in C.

## Prerequisites

- Thru C SDK installed at `~/.thru/sdk/c/`
- Thru toolchain installed at `~/.thru/sdk/toolchain/`

You can install these using:
```bash
thru dev toolchain install
thru dev sdk install c
```

## Building

To build the program:

```bash
make -j
```

The compiled program will be output to:
```
build/thruvm/bin/thru_counter_c.bin
```

## Project Structure

```
thru-counter/
├── GNUmakefile          # Main build configuration
├── README.md            # This file
├── .gitignore           # Git ignore rules
└── examples/
    ├── Local.mk         # Build rules for programs
    └── thru_counter.c  # Program source code
```

## Deploying

To deploy your program to the Thru blockchain, use the `thru` tools:

```bash
# Upload the program
thru uploader upload <seed> build/thruvm/bin/thru_counter_c.bin

# Create a managed program
thru program create <seed> build/thruvm/bin/thru_counter_c.bin
```

## Development

Edit `examples/thru_counter.c` to modify your program logic.

For more information on the Thru C SDK, see the SDK documentation at:
https://docs.thru.org/program-development/setting-up-thru-devkit

## License

See the main Thru network repository for license information.
