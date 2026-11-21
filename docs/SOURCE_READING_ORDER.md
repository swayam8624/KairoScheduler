# Teaching source map — KairoScheduler

CPU task execution and deterministic bounded parallel work contracts.

Follow source links sequentially. For each API/function, identify its input contract, owning data structure, called modules, failure modes, outputs, test and downstream consumers.

## Read in this order

1. [`CMakeLists.txt`](CMakeLists.txt)
2. [`README.md`](README.md)
3. [`src`](src)
4. [`tests`](tests)
5. [`benchmarks`](benchmarks)
6. [`research`](research)

## How this repository interacts with others

MLLibrary and Kairo compute libraries can schedule independent tasks here. Study cancellation, exception propagation, task lifetime and worker coordination before optimizing throughput.

## Verification and study rules

- Read the public C++23 module/entrypoint first, then its implementation and tests.
- Explain invariants, ownership, error states, concurrency or numerical assumptions in comments, not trivial syntax.
- A passing CI job covers only tests actually run on that commit/host; source presence is not runtime certification.
- Check [STATUS.md](STATUS.md) and [STATUS.yaml](STATUS.yaml) for current explicit verification limitations.
- Original work may be modified and extended under the repository license; third-party content retains upstream terms.
