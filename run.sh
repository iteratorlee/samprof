export CUDA_INJECTION64_PATH=`realpath libgpu_profiler_v2.so`

export NO_SAMPLING=1
export DL_BACKEND=TORCH
export PRUNE_CCT=0
export CHECK_RSP=0
export NO_RPC=0
export BT_VERBOSE=0
export DUMP_FN=profiling_results.pb.gz

# <the command to run your application>
