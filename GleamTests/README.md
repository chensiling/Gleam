# GleamTests - Unit Test Suite

## Overview

Unit tests for GleamDebugger using Google Test framework.

## Structure

- `test_main.cpp` - Test entry point
- `test_caches.cpp` - Tests for ILT and symbol caching
- `test_constants.cpp` - Tests for constant definitions
- `CMakeLists.txt` - CMake build configuration

## Building with CMake

```bash
cd GleamTests
mkdir build
cd build
cmake ..
cmake --build .
```

## Running Tests

```bash
./GleamTests
```

## Building with MSBuild

A Visual Studio project file can be generated from CMakeLists.txt or created manually.

## Test Coverage

### Current Tests

1. **Symbol Cache Tests**
   - Empty initialization
   - Store and retrieve
   - Multiple symbols
   - Zero value rejection
   - Cache clearing
   - PDB symbol key format

2. **ILT Cache Tests**
   - Lazy initialization
   - Cache hit detection
   - Multiple modules
   - Cache clearing

3. **Performance Tests**
   - Symbol lookup savings
   - ILT scan savings

4. **Cache Invalidation Tests**
   - Restart scenarios

5. **Constants Tests**
   - Value validation
   - Range checks

### Planned Tests

- Integration tests with real GleamDebugger
- Breakpoint binding tests
- Symbol resolution tests
- ILT disambiguation tests
- Memory operation tests
- Error handling tests

## Notes

- Current tests use mock implementations
- Integration tests require linking with GleamDebugger and GleeBug
- Tests are platform-independent where possible
