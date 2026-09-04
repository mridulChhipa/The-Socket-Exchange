# Assignment 2: The Socket Exchange

## Team

- Student 1: `Mridul Chhipa (2024CS10411)`
- Student 2: `Harsh Verma (2024CS10499)`

## Overview

This project implements the Exchange Server, Trader Client, and Market-Data Client
for COL334 Assignment 2. All communication uses TCP sockets and newline-delimited
text messages.

## Requirements

- Operating system: FreeBSD 14.4-RELEASE or later
- Language: C
- Compiler: `<compiler and version>`
- Runtime/dependencies: `<list any required packages or write "None">`

The server and clients use the POSIX socket API directly. No third-party networking
frameworks are required.

## Build

From the submission root, run:

```sh
make
```

This produces:

- `exchange_server`
- `trader_client`
- `market_data_client`

To remove compiled files:

```sh
make clean
```

## Assumptions
- The username is such that it fits the buffer.
- Using epoll / kqueue for handling concurrent connections and multiplexing.
- Using non-blocking sockets, otherwise the final recv() call would freeze the entire server waiting for data that hasn't arrived yet.

## Implementation Notes

- Concurrency/I/O mechanism: `<select(), poll(), kqueue(), threads, or other>`
- Message-framing approach: `<briefly describe newline-buffer handling>`
- Order-book/matching approach: `<briefly describe>`
- Connection-error handling: `<briefly describe>`

## Design Decisions
