package main

import (
	// "fmt"

	gpuprofiling "code.byted.org/inf/gpu_profiler_client/go-gen"
	"github.com/google/pprof/profile"
)

var (
	cgMap        = make(map[uint64]gpuprofiling.GPUCallingGraph)
	prunedCGMap  = make(map[uint64]*gpuprofiling.GPUCallingGraph)
	cgNodeMap    = make(map[uint64]map[string]*gpuprofiling.GPUCallingGraphNode)
	cgEdgeSrcMap = make(map[uint64]map[string][]*gpuprofiling.GPUCallingGraphEdge)
	cgEdgeDstMap = make(map[uint64]map[string][]*gpuprofiling.GPUCallingGraphEdge)
	// CG after removing zero-weight nodes
)

func getLocationStackV2(trees []*gpuprofiling.CPUCallingContextTree,
	locMap map[uint64]*profile.Location, locStack *map[uint64][]*profile.Location) {
	for _, tree := range trees {
		for _, node := range tree.GetNodeMap() {
			if len(node.GetChildIDs()) == 0 {
				// this node is a leaf node
				parentId := node.GetParentID()
				tmpStack := []*profile.Location{locMap[node.GetId()]}
				for {
					if parentId == 0 {
						break
					}
					tmpStack = append(tmpStack, locMap[parentId])
					parentId = tree.GetNodeMap()[int64(parentId)].GetParentID()
				}
				(*locStack)[node.GetId()] = tmpStack
			}
		}
	}
}

func getGPUCGNodeMap(graph gpuprofiling.GPUCallingGraph) map[string]*gpuprofiling.GPUCallingGraphNode {
	ret := make(map[string]*gpuprofiling.GPUCallingGraphNode)
	for _, node := range graph.GetNodes() {
		ret[node.GetFuncName()] = node
	}
	return ret
}

func getGPUCGEdgeSrcMap(graph gpuprofiling.GPUCallingGraph) map[string][]*gpuprofiling.GPUCallingGraphEdge {
	ret := make(map[string][]*gpuprofiling.GPUCallingGraphEdge)
	for _, edge := range graph.GetEdges() {
		ret[edge.GetSrcFuncName()] = append(ret[edge.GetSrcFuncName()], edge)
	}
	return ret
}

func getGPUCGEdgeDstMap(graph gpuprofiling.GPUCallingGraph) map[string][]*gpuprofiling.GPUCallingGraphEdge {
	ret := make(map[string][]*gpuprofiling.GPUCallingGraphEdge)
	for _, edge := range graph.GetEdges() {
		ret[edge.GetDstFuncName()] = append(ret[edge.GetDstFuncName()], edge)
	}
	return ret
}

func getGPULocationStack(fName string, nodeMap *map[string]*gpuprofiling.GPUCallingGraphNode,
	edgeMap *map[string][]*gpuprofiling.GPUCallingGraphEdge, tmpLocStack *[]*profile.Location,
	locStack *[][]*profile.Location, funcMap *map[string]*profile.Function, locId *uint64) {
	if nextFNames, ok := (*edgeMap)[fName]; ok {
		for _, nextFName := range nextFNames {
			*tmpLocStack = append(*tmpLocStack, &profile.Location{
				ID:      *locId,
				Address: nextFName.GetSrcPCOffset(),
				Line: []profile.Line{{
					// TODO: what if the function does not exist in funcMap
					Function: (*funcMap)[nextFName.GetSrcFuncName()],
					Line:     int64(nextFName.GetSrcPCOffset()),
				}},
			})
			getGPULocationStack(nextFName.GetSrcFuncName(), nodeMap, edgeMap, tmpLocStack, locStack, funcMap, locId)
		}
	} else {
		*locStack = append(*locStack, *tmpLocStack)
		*locId++
	}
}

func setGPUCGWeight(pcSampData *gpuprofiling.CUptiPCSamplingData) {
	for _, pcData := range pcSampData.GetPPcData() {
		cubinCrc := pcData.GetCubinCrc()
		nodes := cgNodeMap[cubinCrc]
		edgesBySrc := cgEdgeSrcMap[cubinCrc]

		// get the sample count of this instruction
		var sampleCount uint64
		for _, stallReason := range pcData.GetStallReason() {
			sampleCount += uint64(stallReason.GetSamples())
		}

		// there might be some functions not in the cg
		// for example:
		//     functionName=Cast_GPU_DT_INT32_DT_FLOAT_kernel
		//     cubinCrc=2991374192
		if _, ok := nodes[pcData.GetFunctionName()]; ok {
			nodes[pcData.GetFunctionName()].Weight += sampleCount
		}
		// } else {
		// 	fmt.Printf("functionName %s not found in %v\n",
		// 		pcData.GetFunctionName(), cubinCrc)
		// }
		for _, edge := range edgesBySrc[pcData.GetFunctionName()] {
			// if the pc sample is a call, add an edge
			if edge.GetSrcPCOffset() == pcData.GetPcOffset() {
				edge.Weight += sampleCount
			}
		}
	}
}

func pruneGPUCG() {
	for cubinCrc, _ := range cgMap {
		q := make([]*gpuprofiling.GPUCallingGraphNode, 0)
		nodes := cgNodeMap[cubinCrc]
		dstEdgeMap := cgEdgeDstMap[cubinCrc]
		for _, node := range nodes {
			if node.GetWeight() > 1 {
				q = append(q, node)
			}
		}

		// propagate the weight of nodes whose weight larger than 1 to their parents
		for len(q) > 0 {
			node := q[0]
			q = q[1:]
			funcName := node.GetFuncName()
			hasCaller := false
			for _, edge := range dstEdgeMap[funcName] {
				if edge.GetWeight() > 1 {
					hasCaller = true
					break
				}
			}
			if !hasCaller {
				for _, edge := range dstEdgeMap[funcName] {
					edge.Weight++
					nodes[edge.GetSrcFuncName()].Weight++
					q = append(q, nodes[edge.GetSrcFuncName()])
				}
			}
		}

		// insert nodes whose weight larger than 1 into prunedCG
		prunedCGMap[cubinCrc] = new(gpuprofiling.GPUCallingGraph)
		// sccNodes := make(map[string]bool)
		for _, node := range nodes {
			if node.GetWeight() > 1 {
				prunedCGMap[cubinCrc].Nodes = append(prunedCGMap[cubinCrc].Nodes, node)
				for _, edge := range dstEdgeMap[node.GetFuncName()] {
					prunedCGMap[cubinCrc].Edges = append(prunedCGMap[cubinCrc].Edges, edge)
				}
			}
		}
	}
}

func sccDfs1(n string, cubinCrc uint64, visited map[string]bool, s []string) {
	visited[n] = true
	for _, edge := range cgEdgeSrcMap[cubinCrc][n] {
		if _, ok := visited[edge.GetDstFuncName()]; !ok {
			sccDfs1(edge.GetDstFuncName(), cubinCrc, visited, s)
		}
	}
	s = append(s, n)
}

func sccDfs2(n string, cubinCrc uint64, color map[string]int, sccCnt *int) {
	color[n] = *sccCnt
	for _, edge := range cgEdgeDstMap[cubinCrc][n] {
		if _, ok := color[edge.GetSrcFuncName()]; !ok {
			sccDfs2(edge.GetSrcFuncName(), cubinCrc, color, sccCnt)
		}
	}
}

func findSCCKosaraju() {
	for cubinCrc, cg := range prunedCGMap {
		visited := make(map[string]bool)
		color := make(map[string]int)
		s := make([]string, 0)
		var sccCnt *int

		for _, node := range cg.GetNodes() {
			if _, ok := visited[node.GetFuncName()]; !ok {
				sccDfs1(node.GetFuncName(), cubinCrc, visited, s)
			}
		}

		for i := len(s) - 1; i >= 0; i-- {
			if _, ok := color[s[i]]; !ok {
				*sccCnt++
				sccDfs2(s[i], cubinCrc, color, sccCnt)
			}
		}
	}
}

func splitCG() {

}

func ProfilingRes2PProf(response *gpuprofiling.GPUProfilingResponse) profile.Profile {
	p := profile.Profile{}
	p.SampleType = []*profile.ValueType{{
		Type: "sample_count",
		Unit: "count",
	}}
	p.Function = []*profile.Function{}
	p.Location = []*profile.Location{}
	p.Sample = []*profile.Sample{}

	cpuCCTs := response.GetCpuCallingCtxTree()
	cpuCCTNodeMap := make(map[int64]*gpuprofiling.CPUCallingContextNode)
	for _, cpuCCT := range cpuCCTs {
		tmpNodeMap := cpuCCT.GetNodeMap()
		for nid, node := range tmpNodeMap {
			cpuCCTNodeMap[nid] = node
		}
	}
	//cpuCCTNodeMap := cpuCCT.GetNodeMap()
	pcSamplingData := response.GetPcSamplingData()

	var funcID, locID uint64
	var maxPC uint64
	funcID, locID = 1, 1
	funcMap := make(map[string]*profile.Function)
	locMap := make(map[uint64]*profile.Location)

	// insert CPU functions & PC locations
	for _, node := range cpuCCTNodeMap {
		if _, ok := funcMap[node.FuncName]; !ok {
			function := profile.Function{
				ID:         funcID,
				Name:       node.FuncName,
				SystemName: node.FuncName,
				StartLine:  0,
			}
			p.Function = append(p.Function, &function)
			funcMap[node.FuncName] = &function
			funcID++
		}

		if _, ok := locMap[node.GetId()]; !ok {
			location := profile.Location{
				ID:      locID,
				Address: node.GetPc(),
				Line: []profile.Line{{
					Function: funcMap[node.FuncName],
					Line:     int64(node.GetOffset()),
				}},
			}
			if maxPC < node.GetPc() {
				maxPC = node.GetPc()
			}
			p.Location = append(p.Location, &location)
			locMap[node.GetId()] = &location
			locID++
		}
	}

	locStack := make(map[uint64][]*profile.Location)
	getLocationStackV2(cpuCCTs, locMap, &locStack)

	// insert GPU functions
	for _, pcSampData := range pcSamplingData {
		for _, pcData := range pcSampData.GetPPcData() {
			if _, ok := funcMap[pcData.FunctionName]; !ok {
				function := profile.Function{
					ID:         funcID,
					Name:       pcData.FunctionName,
					SystemName: pcData.FunctionName,
					StartLine:  0,
				}
				p.Function = append(p.Function, &function)
				funcMap[pcData.FunctionName] = &function
				funcID++
			}
		}
	}

	// load correlated cgs
	for _, pcSampData := range pcSamplingData {
		for _, pcData := range pcSampData.GetPPcData() {
			cubinCrc := pcData.GetCubinCrc()
			if _, ok := cgMap[cubinCrc]; !ok {
				cg := LoadCGByCubinCrc(cubinCrc)
				cgMap[cubinCrc] = cg
			}
			if _, ok := cgNodeMap[cubinCrc]; !ok {
				cgNodeMap[cubinCrc] = getGPUCGNodeMap(cgMap[cubinCrc])
			}
			if _, ok := cgEdgeSrcMap[cubinCrc]; !ok {
				cgEdgeSrcMap[cubinCrc] = getGPUCGEdgeSrcMap(cgMap[cubinCrc])
			}
			if _, ok := cgEdgeDstMap[cubinCrc]; !ok {
				cgEdgeDstMap[cubinCrc] = getGPUCGEdgeDstMap(cgMap[cubinCrc])
			}
		}
	}

	// convert a GPU cg to a GPU cct
	for _, pcSampData := range pcSamplingData {
		setGPUCGWeight(pcSampData)
	}

	pruneGPUCG()
	// update node map and edge map
	for cubinCrc, cg := range prunedCGMap {
		cgNodeMap[cubinCrc] = getGPUCGNodeMap(*cg)
		cgEdgeSrcMap[cubinCrc] = getGPUCGEdgeSrcMap(*cg)
		cgEdgeDstMap[cubinCrc] = getGPUCGEdgeDstMap(*cg)
	}
	findSCCKosaraju()
	splitCG()

	// insert GPU PC locations and PC samples
	for _, pcSampData := range pcSamplingData {
		for _, pcData := range pcSampData.GetPPcData() {
			location := profile.Location{
				ID:      locID,
				Address: maxPC + 1 + pcData.GetPcOffset(),
				Line: []profile.Line{{
					Function: funcMap[pcData.FunctionName],
					Line:     int64(pcData.GetPcOffset()),
				}},
			}
			p.Location = append(p.Location, &location)
			locID++

			parentCPUPCID := pcData.GetParentCPUPCID()
			var locationStack []*profile.Location

			// get the GPU call graph
			cubinCrc := pcData.GetCubinCrc()
			nodeMap := cgNodeMap[cubinCrc]
			edgeMap := cgEdgeDstMap[cubinCrc]

			if _, ok := edgeMap[pcData.GetFunctionName()]; !ok {
				locationStack = append(locationStack, &location)
			} else {
				var gpuLocationStacks [][]*profile.Location
				var tmpStack []*profile.Location
				getGPULocationStack(pcData.GetFunctionName(), &nodeMap, &edgeMap, &tmpStack,
					&gpuLocationStacks, &funcMap, &locID)
				locationStack = append(locationStack, &location)
				for _, gpuLocationStack := range gpuLocationStacks {
					locationStack = append(locationStack, gpuLocationStack...)
				}
			}
			locationStack = append(locationStack, locStack[uint64(parentCPUPCID)][1:]...)

			// calculate the sample counts of the GPU PC
			var sampleCount int64
			for _, stallReason := range pcData.GetStallReason() {
				sampleCount += int64(stallReason.GetSamples())
			}

			// insert a pprof sample
			p.Sample = append(p.Sample, &profile.Sample{
				Location: locationStack,
				Value:    []int64{sampleCount},
			})
		}
	}

	return p
}
