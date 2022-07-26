Application-oblivious GPU profiler for CUDA applications.

## 1. Dependency
```
CUDA(CUPTI)=11.6, grpc & protobuf, libunwind8-dev, libpython3.8-dev
```
### 1.1 Install grpc & protobuf from source
Please refer to [gRPC-C++ Quick Start](https://grpc.io/docs/languages/cpp/quickstart/)
### 1.2 Install libunwind
```bash
apt install libunwind8-dev
```
### 1.3 Configuring
```bash
# Make sure CUDA lib path and grpc lib path have been appended to LD_LIBRARY_PATH
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/path/to/cuda/lib64:/path/to/grpc/lib
# Make sure /path/to/Python.h has been appended to CPLUS_INCLUDE_PATH
# If you could not find Python.h, consider installing libpython3.8-dev
export CPLUS_INCLUDE_PATH=$LD_LIBRARY_PATH:/path/to/grpc/include:/usr/include/python3.8
export PKG_CONFIG_PATH=/path/to/grpc/lib/pkgconfig
```

## 2. Build
```bash
# Build the default version of Samprof, the output would be libgpu_profiler_v2.so
make
```

## 3. Usage

Samprof-defined environment variables are listed in the following table:

| Name | Type | Description | Default |
| :--- | :--- | :--- | :--- |
| `CUDA_INJECTION64_PATH` | string | Path to `libgpu_profiler_\<version>.so` | explicitly set by user |
| `DL_BACKEND` | string | **TORCH**: Pytorch <br> **TF**: Tensorflow | TORCH |
| `NO_SAMPLING` | bool | **0[DEV]**: profiling based on pc sampling, the hybrid CCT could be inaccurate, and remote profiling could be stuck due to CUPTI internal bugs <br> **1**: profiling based on tracing instead of pc sampling, binding timers around CUDA API calls to record CUDA kernels | **0** |
| `NO_RPC` | bool | **0**: starting a standby rpc server, remote profiling request could be issued using client <br> **1**: profiling the application for the whole life-cycle and saving the profiling results to `DUMP_FN` | **0** |
| `DUMP_FN` | string | the path of the file to save the profiling results, only work when `NO_RPC` is set to **1** | |
| `CHECK_RSP` | bool | **0**: not checking the *%rsp* register before call stack unwinding, the CPU CCT is guaranteed to be accurate <br> **1**: checking the *%rsp* register before call stack unwinding, the CPU CCT could be inaccurate, while the overhead could be reduced significantly | **1** |
| `PRUNE_CCT` | bool | **0**: returning the complete CCT <br> **1**: returning trimmed CCT pruned by Samprof-defined rules | **1** |
| `BT_VERBOSE` | bool | **0**: not printing the detailed call path upon call stack unwinding <br> **1**: printing the detailed call path upon call stack unwinding | **0** |

We provide a helper script `run.sh`. Configure the environments according to your needs. Then running `bash run.sh` would work.

If `NO_RPC` is set to **0**, a profiler RPC server would be started at `0.0.0.0:8886` by default. An RPC client is provided in the `bins` directory. Running `./gpu_profiler_client --duration 2000` would issue a two seconds profiling request to `localhost:8886` and perform analysis on the response.

If `NO_RPC` is set to **1**, the profiling result would be dumped to an intermediate file (indicated by `DUMP_FN`). In this case, running `./gpu_profiler_client --pbfn $DUMP_FN` for further analysis.

After running the client successfully, the analysis results would be saved in `profiler.pb.gz`. Running `pprof -http=0.0.0.0:<port> --trim=false --call_tree ./profiler.pb.gz` would start a pprof web server at `0.0.0.0:<port>` which you can visit via a web browser. In case that the call graph is too large, your could run `pprof -pdf --trim=false --call_tree ./profiler.pb.gz >> profile.pdf` to save the call graph to a pdf file.

If you want to customize the parameters, just run `./gpu_profiler_client -h` for the help information.

## 4. Tools

### 4.1 C++ RPC Client
```bash
# Build the C++ RPC Client
make client_cpp
# Issue a profiling request
./client_cpp <ip>:<port> <duration>
```
The C++ RPC client would not analyze the profiling results and generate pprof format profiles. It simply prints the RPC response. Consider use it for debugging.

### 4.2 Cubin Extraction (Offline Mode)
In the offline mode, Samprof extracts all the loaded modules to cubin files at runtime. The extracted cubin file would be saved as `<moduleId>.cubin`. Setting the `OFFLINE` macro to 1 would enable the offline mode.
```cpp
// gpu_profiler.cpp line 842
#define OFFLINE 1
```

### 4.3 Static Cubin Analysis
```bash
# Build the cubin analyzer
make cubin_tool
```
`cubin_tool` analyzes the calling relations between CUDA functions via analyzing the assembly code of cubins. Running `./cubin_tool <cubin path> <cubin call graph path>` would analyze all the cubin files in `<cubin path>` and save the analysis results to `<cubin call graph path>`. Call graph files are named as `<cubin crc>.pb.gz`. The proto of CUDA call graph is defined in [gpu_profiling.proto](https://github.com/iteratorlee/samprof/blob/master/protos/gpu_profiling.proto).

Via setting the `PRINTGRAPH` macro to `true`, you could print all the call graph files by running `./cubin_tool <cubin call graph path>`