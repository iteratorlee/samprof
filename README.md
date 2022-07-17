Application-oblivious GPU profiler for CUDA applications.

## Dependency
```
CUDA(CUPTI)=11.6, grpc & protobuf, libunwind
```

## Build
```
make
```

## Usage
```
export CUDA_INJECTION64_PATH=/path/to/libgpu_profiler.so
```
Then, just run your application as usual.

The PC sampling RPC server is started at `0.0.0.0:50051` by default. An RPC client is provided to for testing usage. Running `make client` would generate the client.

## Features

| Feature | Intro | Status |
| :---- | :---- | :---- |
| GPU PC sampling | The profiler would start PC sampling upon receiving <br> a request from the rpc client | WIP |
| Hybrid (CPU & GPU) <br> calling context tree <br> (CCT) reconstruction | The profiler maintains a CPU calling context tree via <br> event-based CPU call stack unwinding. The GPU PC <br> samples are inserted as leaf nodes of the tree. Once <br> sent back to the client, the hybrid CCT would be re-<br>constructed using a dedicated algorithm. | WIP |