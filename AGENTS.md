# Build & Test
```bash
ninja -C build -j$(nproc)
for t in build/tests/test_*; do if [ -x "$t" ] && [ ! -d "$t" ]; then "$t"; fi; done
```
