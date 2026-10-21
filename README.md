# Linkway

A low-latency messaging library for C++17 on Linux. It lets components in the same process or on the same machine talk to each other with typed publish/subscribe and request/reply.

## Features

- Typed **publish/subscribe** and **request/reply** (async `request()` or synchronous `call()`)
- **Shared-memory transport** with zero-copy publish (`reserve<T>()`), plus a stream transport
- Optional poll mode (run-to-completion on your own thread) and inline dispatch for the fast path
- Optional CRC32C checksum on every frame
- Built-in metrics and a `SIGUSR1` state dump for debugging

## Example

```cpp
#include "linkway/bus.hpp"

struct Price { std::uint64_t id; double value; };
LINKWAY_MESSAGE(Price, 1)

auto bus = linkway::Bus::create(transport).value();
bus->subscribe<Price>([](const Price& p) { /* handle */ });
bus->publish(Price{42, 101.5});
```

## Build

Requirements: Linux, CMake 3.16+, GCC 8+ or Clang (C++17).

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build
```

Useful options: `-DLINKWAY_ASAN=ON`, `-DLINKWAY_UBSAN=ON`, `-DLINKWAY_TSAN=ON`, `-DLINKWAY_TESTS=OFF`.

To use it from another CMake project after `cmake --install build`:

```cmake
find_package(linkway REQUIRED)
target_link_libraries(my_app PRIVATE linkway::linkway)
```

## Status

Work in progress: the code is being pushed to this repository in stages.

## License

MIT
