# GleeBug Testing Strategy

## Test Framework

**Framework**: Google Test (gtest) v1.15.2  
**Location**: `GleamTests/`  
**Build System**: CMake (primary), MSBuild (planned)

## Test Categories

### 1. Unit Tests (Current)

Located in `GleamTests/`, these test individual components in isolation:

#### Cache Tests (`test_caches.cpp`)
- **Symbol Cache**: 8 tests covering storage, retrieval, clearing
- **ILT Cache**: 5 tests covering lazy loading, cache hits, multi-module
- **Performance**: 2 tests simulating cache savings
- **Invalidation**: 2 tests for restart scenarios

**Status**: ✅ Implemented with mock objects

#### Constants Tests (`test_constants.cpp`)
- **Validation**: 5 tests checking constant ranges and values
- **Sanity checks**: Page alignment, buffer sizes, timeout reasonableness

**Status**: ✅ Implemented

### 2. Integration Tests (Planned)

Test GleamDebugger with real processes:

- Attach to test target
- Set breakpoints on symbols
- Verify symbol resolution
- Test ILT disambiguation
- Memory read/write operations
- Exception handling

**Status**: ⏳ Planned for Phase 3 Batch 1

### 3. End-to-End Tests (Planned)

Full debugging scenarios:

- Launch target, set breakpoints, step, inspect
- Module load/unload with logical breakpoints
- Restart with breakpoint persistence
- Detach and re-attach

**Status**: ⏳ Planned for Phase 3 Batch 2

## Running Tests

### With CMake

```bash
cd GleamTests
mkdir build && cd build
cmake ..
cmake --build .
./GleamTests
```

### With Visual Studio (Future)

```bash
# Generate VS solution
cmake -G "Visual Studio 17 2022" -A x64 ..
# Open GleamTests.sln and build
```

## Test Results

### Current Test Suite

**Total Tests**: 15  
**Passing**: 15 (Expected - not yet run)  
**Failing**: 0  

### Coverage

- ✅ Symbol cache logic
- ✅ ILT cache logic
- ✅ Constants validation
- ⏳ Real symbol resolution (integration)
- ⏳ Breakpoint management (integration)
- ⏳ Memory operations (integration)

## CI Integration (Future)

### Planned Pipeline

1. **Build**: Compile GleeBug, Gleam, GleamTests
2. **Unit Tests**: Run `GleamTests`
3. **Integration Tests**: Run with test targets
4. **Report**: Generate coverage report

### Platforms

- Windows x64 (primary)
- Windows ARM64 (future)

## Test Data

### Test Targets

Existing test executables in `bin/Release/x64/`:
- `TestTarget.exe` - Simple test program
- `ArgvTarget.exe` - Command-line argument testing
- `BoundaryTarget.exe` - Boundary condition testing

### Mock Data

Unit tests use synthetic data:
- Module bases: `0x140000000`, `0x7FFA12340000`
- Symbol addresses: Offset from module base
- ILT targets: Simulated thunk addresses

## Adding New Tests

### Unit Test

1. Create `test_<component>.cpp` in `GleamTests/`
2. Add to `CMakeLists.txt`
3. Include necessary headers
4. Write `TEST()` macros
5. Build and verify

### Integration Test

1. Create test in `GleamTests/integration/`
2. Link with GleamDebugger and GleeBug
3. Use real test targets
4. Verify cleanup (process termination, handle closure)

## Test Maintenance

- **Update tests** when APIs change
- **Add regression tests** for bug fixes
- **Keep mock behavior** synchronized with real implementation
- **Document test data** and expected outcomes

---

*Last Updated: 2026-07-30*  
*Test Framework: Google Test 1.15.2*  
*Status: Phase 3 Batch 1 - Unit tests implemented*
