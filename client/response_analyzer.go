package main

import gpuprofiling "code.byted.org/inf/gpu_profiler_client/go-gen"
import "strings"
//import "fmt"

func getStallReasonDistribution(r *gpuprofiling.GPUProfilingResponse) map[string][]int {
    dist := make(map[string][]int)
    // calculate the distribution of stall reasons
    return dist
}
func getCUDAKernelDistribution(r *gpuprofiling.GPUProfilingResponse) map[string]int {
    // calculate the distribution of CUDA kernels
    dist := make(map[string]int)

    pcSamplingData := r.GetPcSamplingData()
    for _, pcSampData := range pcSamplingData {
        for _, pcData := range pcSampData.GetPPcData() {
            var sampleCount int = 0
            for _, stallReason := range pcData.GetStallReason() {
                sampleCount += int(stallReason.GetSamples())
            }
            funcName := pcData.GetFunctionName();
            if _,ok := dist[funcName]; ok{
                dist[funcName]+=sampleCount
            } else{
                dist[funcName]=sampleCount
            }
        }
    }

    return dist
}

func isValidOPName(s string) bool {
    return strings.Contains(s,"ops")
    //return len(s) > 0
    //return strings.Contains(s, "op")
}

// return the root's sample count and record it in the dist
func dfsCCTSampleCount(rootId int64, id2node map[int64]*gpuprofiling.CPUCallingContextNode, id2cnt map[int64]int)int {
    var sampleCount int = 0
    root := id2node[rootId]
    if childIds := root.GetChildIDs(); len(childIds) >0{
        // non-leaf node
        for _,childId := range childIds{
            sampleCount += dfsCCTSampleCount(int64(childId), id2node,id2cnt)
        }
    }else{
        // leaf node
        sampleCount = id2cnt[rootId]
    }

    // node id is globally unique
    id2cnt[rootId]=sampleCount

    return sampleCount
}

func getOPDistribution(r *gpuprofiling.GPUProfilingResponse) map[string]int {
    // calculate the distribution of OPs

    dist := make(map[string]int)
    // sample count of leaf nodes
    nid2cnt := make(map[int64]int)
    pcSamplingData := r.GetPcSamplingData()
    for _, pcSampData := range pcSamplingData {
        for _, pcData := range pcSampData.GetPPcData() {
            var sampleCount int = 0
            for _, stallReason := range pcData.GetStallReason() {
                sampleCount += int(stallReason.GetSamples())
            }

            parentId := pcData.GetParentCPUPCID()
            if _, ok := nid2cnt[parentId];ok{
                nid2cnt[parentId] += sampleCount
            }else{
                nid2cnt[parentId] = sampleCount
            }
        }
    }

    cpuCCTs := r.GetCpuCallingCtxTree()
    nid2name := make(map[int64]string)
    //Count the number of samples according to nid, in case some ops appear on multiple links
    for _, cpuCCT := range cpuCCTs {
        rootId := int64(cpuCCT.GetRootID())
        nodeMap := cpuCCT.GetNodeMap()
        for tmpId,tmpNode := range nodeMap{
            nid2name[tmpId]=tmpNode.GetFuncName()
        }
        dfsCCTSampleCount(rootId,nodeMap, nid2cnt);
    }

    for tmpId, tmpName :=range nid2name{
        if !isValidOPName(tmpName){
            continue
        }
        if _,ok := dist[tmpName]; ok{
            dist[tmpName]+=nid2cnt[tmpId]
        }else{
            dist[tmpName]=nid2cnt[tmpId]
        }
    }
    return dist
}

func getLayerDistribution(r *gpuprofiling.GPUProfilingResponse) map[string]int {
    dist := make(map[string]int)
    // calculate the distribution of Layers
    return dist
}
