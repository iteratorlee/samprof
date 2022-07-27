#!/usr/bin/zsh
protoc --go_out=./go-gen --go_opt=paths=source_relative \
    --go-grpc_out=./go-gen --go-grpc_opt=paths=source_relative \
    gpu_profiling.proto
