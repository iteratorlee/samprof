ifndef OS
    OS   := $(shell uname)
    HOST_ARCH := $(shell uname -m)
endif

CUDA_INSTALL_PATH ?= /usr/local/cuda
NVCC := "$(CUDA_INSTALL_PATH)/bin/nvcc"
INCLUDES := -I"$(CUDA_INSTALL_PATH)/include"

LIB_PATH ?= $(CUDA_INSTALL_PATH)/lib64

NVCCFLAGS :=
NVCCFLAGS += -std=c++11
# replace -pthread with -lpthread in case that NVCC does not recognize
GRPC_LDFLAGS = $(subst -pthread,-lpthread,$(shell pkg-config --libs protobuf grpc++))
LDFLAGS += -L/usr/local/lib
LDFLAGS += $(GRPC_LDFLAGS)
LDFLAGS += -lgrpc++_reflection\

PROTOC = protoc
GRPC_CPP_PLUGIN = grpc_cpp_plugin
GRPC_CPP_PLUGIN_PATH ?= `which $(GRPC_CPP_PLUGIN)`

PROTOS_PATH = ./protos
GRPC_CPP_GEN_PATH = ./cpp-gen
GRPC_GO_GEN_PATH = ./go-gen

vpath %.proto $(PROTOS_PATH)
vpath % $(GRPC_CPP_GEN_PATH)
vpath % $(GRPC_GO_GEN_PATH)

ifeq ($(OS), Darwin)
    export DYLD_LIBRARY_PATH := $(DYLD_LIBRARY_PATH):$(LIB_PATH)
    LIBS= -Xlinker -framework -Xlinker cuda -L $(LIB_PATH) -lcupti -lpcsamplingutil
else
    export LD_LIBRARY_PATH := $(LD_LIBRARY_PATH):$(LIB_PATH)
    LIBS = -L $(LIB_PATH) -lcuda -lcupti -lpcsamplingutil -lunwind -lpython3.8 -lpthread -ldl
endif
LIBNAMEV1 := libgpu_profiler_v1.so
LIBNAMEV2 := libgpu_profiler_v2.so
LIBNAME_NO_RPC := libgpu_profiler_no_rpc.so
NVCCFLAGS += -Xcompiler -fPIC

ifneq ($(TARGET_ARCH), $(HOST_ARCH))
    ifeq ($(TARGET_ARCH), aarch64)
        ifeq ($(TARGET_OS), linux)
            HOST_COMPILER ?= aarch64-linux-gnu-g++
        endif
    endif

    ifdef HOST_COMPILER
        NVCCFLAGS += -ccbin $(HOST_COMPILER)
    endif
endif

all: gpu_profiler

gpu_profiler: gpu_profiler.cpp cpp-gen/gpu_profiling.pb.cc cpp-gen/gpu_profiling.grpc.pb.cc common.cpp cpu_sampler.cpp
	$(NVCC) -g $(NVCCFLAGS) $(INCLUDES) -o $(LIBNAMEV2) -shared $^ $(LIBS) $(LDFLAGS)

gpu_profiler_debug: gpu_profiler.cpp cpp-gen/gpu_profiling.pb.cc cpp-gen/gpu_profiling.grpc.pb.cc common.cpp cpu_sampler.cpp
	$(NVCC) -g $(NVCCFLAGS) $(INCLUDES) -o profiler_debug $^ $(LIBS) $(LDFLAGS)

gpu_profiler_wo_rpc: gpu_profiler_wo_rpc.cpp
	$(NVCC) -g $(NVCCFLAGS) $(INCLUDES) -o $(LIBNAME_NO_RPC) -shared $^ $(LIBS)

gpu_profiler_wo_rpc_debug: gpu_profiler_wo_rpc.cpp
	$(NVCC) -g $(NVCCFLAGS) $(INCLUDES) -o profiler_debug $^ $(LIBS)

gpu_profiler_old: gpu_profiler_old_version.cpp cpp-gen/gpu_profiling.pb.cc cpp-gen/gpu_profiling.grpc.pb.cc common.cpp back_tracer.cpp
	$(NVCC) -g $(NVCCFLAGS) $(INCLUDES) -o $(LIBNAMEV1) -shared $^ $(LIBS) $(LDFLAGS)

client_cpp: tools/client.cpp cpp-gen/gpu_profiling.pb.cc cpp-gen/gpu_profiling.grpc.pb.cc
	$(NVCC) -std=c++11 $^ -o $@ $(LDFLAGS)

cubin_tool: tools/cubin_tool.cpp tools/get_cubin_crc.cpp cpp-gen/gpu_profiling.pb.cc cpp-gen/gpu_profiling.grpc.pb.cc
	$(NVCC) -g -std=c++11 $^ -o $@ $(LIBS) $(LDFLAGS)

test: test.cpp common.cpp back_tracer.cpp cpu_sampler.cpp
	$(NVCC) -forward-unknown-to-host-compiler -rdynamic -g -std=c++11 $^ -o $@ $(LIBS)

.PRECIOUS: $(GRPC_CPP_GEN_PATH)/%.pb.cc
cpp-gen/%.pb.cc: %.proto
	$(PROTOC) -I $(PROTOS_PATH) --cpp_out=$(GRPC_CPP_GEN_PATH) $<

.PRECIOUS: $(GRPC_CPP_GEN_PATH)/%.grpc.pb.cc
cpp-gen/%.grpc.pb.cc: %.proto
	$(PROTOC) -I $(PROTOS_PATH) --grpc_out=$(GRPC_CPP_GEN_PATH) --plugin=protoc-gen-grpc=$(GRPC_CPP_PLUGIN_PATH) $<
clean:
	rm $(LIBNAME)
