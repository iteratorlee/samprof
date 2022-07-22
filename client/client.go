package main

import (
	"bufio"
	"context"
	"flag"
	"fmt"
	"io"
	"io/ioutil"
	"log"
	"os"
	"time"

	gpuprofiling "code.byted.org/inf/gpu_profiler_client/go-gen"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/protobuf/proto"
)

var (
	addr     = flag.String("addr", "localhost:8886", "the address to connect to")
	duration = flag.Uint("duration", 2000, "PC sampling duration")
	dumpfn   = flag.String("dumpfn", "", "the dumped profile filename")
	pbfn     = flag.String("pbfn", "", "the dumped pb profile filename")
)

func getProfilingResponseFromFile(s string) *gpuprofiling.GPUProfilingResponse {
	f, err := os.Open(s)
	if err != nil {
		log.Fatalf("could not open dump file: %v", err)
		return nil
	}
	defer f.Close()

	r := new(gpuprofiling.GPUProfilingResponse)
	br := bufio.NewReader(f)
	for {
		line, _, c := br.ReadLine()
		if c == io.EOF {
			break
		}
		fmt.Println(line)
		tree := new(gpuprofiling.CPUCallingContextTree)
		r.CpuCallingCtxTree = append(r.CpuCallingCtxTree, tree)
		node := new(gpuprofiling.CPUCallingContextNode)
		r.CpuCallingCtxTree[len(r.CpuCallingCtxTree)-1].NodeMap[1] = node
	}

	return r
}

func getProfilingResponseFromPB(s string) *gpuprofiling.GPUProfilingResponse {
	f, err := os.Open(s)
	if err != nil {
		log.Fatalf("could not open pb file: %v", err)
		return nil
	}
	defer f.Close()

	r := new(gpuprofiling.GPUProfilingResponse)
	data, err := ioutil.ReadAll(f)
	if err != nil {
		log.Fatalf("could not read from pb file: %v", err)
		return nil
	}

	err = proto.Unmarshal(data, r)
	if err != nil {
		log.Fatalf("unmarshal failed: %v", err)
		return nil
	}

	return r
}

func main() {
	flag.Parse()
	r := new(gpuprofiling.GPUProfilingResponse)

	if *pbfn != "" {
		log.Printf("loading response from pre-dumped pb: %v", *pbfn)
		r = getProfilingResponseFromPB((*pbfn))
	} else if *dumpfn != "" {
		log.Printf("loading response from pre-dumped txt: %v", *dumpfn)
		r = getProfilingResponseFromFile(*dumpfn)
	} else {
		log.Printf("no dumped file, issuing rpc request")
		maxSizeOption := grpc.MaxCallRecvMsgSize(1024 * 1024 * 64)
		conn, err := grpc.Dial(*addr, grpc.WithTransportCredentials(insecure.NewCredentials()))
		if err != nil {
			log.Fatalf("did not connect: %v", err)
		}
		defer conn.Close()
		c := gpuprofiling.NewGPUProfilingServiceClient(conn)

		ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
		defer cancel()
		r, err = c.PerformGPUProfiling(ctx, &gpuprofiling.GPUProfilingRequest{Duration: uint32(*duration)}, maxSizeOption)
		if err != nil {
			log.Fatalf("could not perform profiling: %v", err)
		}
	}

	// log.Printf("Response: %v", r.GetCpuCallingCtxTree())
	// log.Printf("PProf profile: %v", ProfilingRes2PProf(r))
	p := ProfilingRes2PProf(r)
	f, err := os.Create("profile.pb.gz")
	if err != nil {
		log.Fatalf("could not create file: %v", err)
	}
	if err := p.Write(f); err != nil {
		log.Fatalf("could not dump file: %v", err)
	}
	log.Printf("profile dumped to profile.pb.gz successfully!")
}
