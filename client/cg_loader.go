package main

import (
	"flag"
	"fmt"
	"io/ioutil"
	"log"

	gpuprofiling "code.byted.org/inf/gpu_profiler_client/go-gen"
	"github.com/golang/protobuf/proto"
)

var cgStorePath = flag.String("cgpath", "/root/repos/gpu_profiler/cu_samples/cg_store", "path to cg files, organized by cubin crc")

func LoadCGFromFile(cgFilePath string) gpuprofiling.GPUCallingGraph {
	var graph gpuprofiling.GPUCallingGraph
	in, err := ioutil.ReadFile(cgFilePath)
	if err != nil {
		//log.Fatalf("can not read cg from %s: %v", cgFilePath, err)
		return graph
	}

	err = proto.Unmarshal(in, &graph)
	if err != nil {
		log.Fatalf("can not load cg from file: %v", err)
	}

	return graph
}

func LoadCGByCubinCrc(cubinCrc uint64) gpuprofiling.GPUCallingGraph {
	cgFilePath := fmt.Sprint(*cgStorePath, "/", cubinCrc, ".pb.gz")
	return LoadCGFromFile(cgFilePath)
}
